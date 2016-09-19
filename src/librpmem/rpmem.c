/*
 * Copyright 2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * rpmem.c -- main source file for librpmem
 */
#include <stdlib.h>
#include <netdb.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include "librpmem.h"
#include "out.h"
#include "util.h"
#include "rpmem_common.h"
#include "rpmem_util.h"
#include "rpmem_obc.h"
#include "rpmem_fip.h"
#include "rpmem_fip_common.h"
#include "rpmem_ssh.h"

/*
 * rpmem_pool -- remote pool context
 */
struct rpmem_pool {
	struct rpmem_obc *obc;		/* out-of-band connection handle */
	struct rpmem_fip *fip;		/* fabric provider handle */
	struct rpmem_target_info *info;
	char fip_service[NI_MAXSERV];
	enum rpmem_provider provider;
	pthread_t monitor;

	/*
	 * Last error code, need to be volatile because it can
	 * be accessed by multiple threads.
	 */
	volatile int error;
};

/*
 * env_get_bool -- parse value of specified environment variable as a bool
 *
 * Return values:
 *  0 - defined, valp has value
 *  1 - not defined
 * -1 - parsing error
 */
static int
env_get_bool(const char *name, int *valp)
{
	const char *env = getenv(name);
	if (!env)
		return 1;

	char *endptr;
	errno = 0;
	long val = strtol(env, &endptr, 10);
	if (*endptr != '\0' || errno)
		goto err;

	if (val < INT_MIN || val > INT_MAX)
		goto err;

	*valp = (int)val;

	return 0;
err:
	RPMEM_LOG(ERR, "!parsing '%s' environment variable failed", name);
	return -1;
}

/*
 * rpmem_get_provider -- returns provider based on node address and environment
 */
static enum rpmem_provider
rpmem_get_provider(const char *node)
{
	struct rpmem_fip_probe probe;
	enum rpmem_provider prov = RPMEM_PROV_UNKNOWN;

	int ret = rpmem_fip_probe_get(node, &probe);
	if (ret)
		return prov;

	/*
	 * The sockets provider can be used only if specified environment
	 * variable is set to 1.
	 */
	if (rpmem_fip_probe(probe, RPMEM_PROV_LIBFABRIC_SOCKETS)) {
		int enable;
		ret = env_get_bool(RPMEM_PROV_SOCKET_ENV, &enable);
		if (!ret && enable) {
			prov = RPMEM_PROV_LIBFABRIC_SOCKETS;
		}
	}

	/*
	 * The verbs provider is enabled by default. If appropriate
	 * environment variable is set to 0, the verbs provider is disabled.
	 *
	 * The verbs provider has higher priority than sockets provider.
	 */
	if (rpmem_fip_probe(probe, RPMEM_PROV_LIBFABRIC_VERBS)) {
		int enable;
		ret = env_get_bool(RPMEM_PROV_VERBS_ENV, &enable);
		if (ret == 1 || (!ret && enable))
			prov = RPMEM_PROV_LIBFABRIC_VERBS;
	}

	return prov;

}

/*
 * rpmem_monitor_thread -- connection monitor background thread
 */
static void *
rpmem_monitor_thread(void *arg)
{
	RPMEMpool *rpp = arg;

	int ret = rpmem_obc_monitor(rpp->obc, 0);
	if (ret) {
		rpp->error = errno;
	}

	return NULL;
}

/*
 * rpmem_common_init -- common routine for initialization
 */
static RPMEMpool *
rpmem_common_init(const char *target)
{
	int ret;

	RPMEMpool *rpp = calloc(1, sizeof(*rpp));
	if (!rpp) {
		ERR("!calloc");
		goto err_malloc_rpmem;
	}

	rpp->info = rpmem_target_parse(target);
	if (!rpp->info) {
		ERR("!parsing target node address failed");
		goto err_target_split;
	}

	rpp->provider = rpmem_get_provider(rpp->info->node);
	if (rpp->provider == RPMEM_PROV_UNKNOWN) {
		errno = ENOMEDIUM;
		ERR("cannot find provider");
		goto err_provider;
	}

	RPMEM_LOG(NOTICE, "provider: %s", rpmem_provider_to_str(rpp->provider));

	if (rpp->provider == RPMEM_PROV_LIBFABRIC_SOCKETS) {
		/* libfabric's sockets provider does not support IPv6 */
		RPMEM_LOG(NOTICE, "forcing using IPv4");
		rpp->info->flags |= RPMEM_FLAGS_USE_IPV4;
	}

	rpp->obc = rpmem_obc_init();
	if (!rpp->obc) {
		ERR("!out-of-band connection initialization failed");
		goto err_obc_init;
	}

	RPMEM_LOG(INFO, "establishing out-of-band connection");

	ret = rpmem_obc_connect(rpp->obc, rpp->info);
	if (ret) {
		ERR("!out-of-band connection failed");
		goto err_obc_connect;
	}

	RPMEM_LOG(NOTICE, "out-of-band connection established");

	return rpp;
err_obc_connect:
	rpmem_obc_fini(rpp->obc);
err_obc_init:
err_provider:
	rpmem_target_free(rpp->info);
err_target_split:
	free(rpp);
err_malloc_rpmem:
	return NULL;
}

/*
 * rpmem_remove_pool -- remove pool on remote node
 */
static int
rpmem_remove_pool(const struct rpmem_target_info *info, const char *pool_set)
{
	RPMEM_LOG(NOTICE, "removing pool -- '%s'", pool_set);
	struct rpmem_ssh *ssh = rpmem_ssh_run(info, "--remove",
			pool_set, NULL);
	if (!ssh) {
		RPMEM_LOG(ERR, "executing remove command failed");
		return -1;
	}

	int ret = 0;

	if (rpmem_ssh_monitor(ssh, 0)) {
		RPMEM_LOG(ERR, "waiting for remove command failed");
		ret = -1;
		goto err_monitor;
	}

err_monitor:
	rpmem_ssh_close(ssh);

	return ret;
}

/*
 * rpmem_common_fini -- common routing for deinitialization
 */
static void
rpmem_common_fini(RPMEMpool *rpp, int join, const char *pool_set)
{
	rpmem_obc_disconnect(rpp->obc);

	if (join) {
		int ret = pthread_join(rpp->monitor, NULL);
		if (ret) {
			errno = ret;
			ERR("joining monitor thread failed");
		}
	}

	rpmem_obc_fini(rpp->obc);

	if (pool_set) {
		if (rpmem_remove_pool(rpp->info, pool_set))
			RPMEM_LOG(ERR, "removing '%s' pool failed",
					pool_set);
	}

	rpmem_target_free(rpp->info);
	free(rpp);
}

/*
 * rpmem_common_fip_init -- common routine for initializing fabric provider
 */
static int
rpmem_common_fip_init(RPMEMpool *rpp, struct rpmem_req_attr *req,
	struct rpmem_resp_attr *resp, void *pool_addr, size_t pool_size,
	unsigned *nlanes)
{
	int ret;

	struct rpmem_fip_attr fip_attr = {
		.provider	= req->provider,
		.persist_method	= resp->persist_method,
		.laddr		= pool_addr,
		.size		= pool_size,
		.nlanes		= min(*nlanes, resp->nlanes),
		.raddr		= (void *)resp->raddr,
		.rkey		= resp->rkey,
	};

	ssize_t sret = snprintf(rpp->fip_service, sizeof(rpp->fip_service),
			"%u", resp->port);
	if (sret <= 0) {
		ERR("!snprintf");
		ret = (int)sret;
		goto err_port;
	}

	rpp->fip = rpmem_fip_init(rpp->info->node, rpp->fip_service,
			&fip_attr, nlanes);
	if (!rpp->fip) {
		ERR("!in-band connection initialization failed");
		ret = -1;
		goto err_fip_init;
	}

	RPMEM_LOG(INFO, "establishing in-band connection");

	ret = rpmem_fip_connect(rpp->fip);
	if (ret) {
		ERR("!establishing in-band connection failed");
		goto err_fip_connect;
	}

	RPMEM_LOG(NOTICE, "in-band connection established");

	ret = rpmem_fip_process_start(rpp->fip);
	if (ret) {
		ERR("!starting in-band connection thread");
		goto err_fip_process;
	}

	RPMEM_LOG(NOTICE, "final nlanes: %u", *nlanes);

	return 0;
err_fip_process:
	rpmem_fip_close(rpp->fip);
err_fip_connect:
	rpmem_fip_fini(rpp->fip);
err_fip_init:
err_port:
	return ret;
}

/*
 * rpmem_common_fip_fini -- common routine for deinitializing fabric provider
 */
static void
rpmem_common_fip_fini(RPMEMpool *rpp)
{
	RPMEM_LOG(INFO, "closing in-band connection");

	rpmem_fip_process_stop(rpp->fip);
	rpmem_fip_close(rpp->fip);
	rpmem_fip_fini(rpp->fip);

	RPMEM_LOG(NOTICE, "in-band connection closed");
}

/*
 * rpmem_log_args -- log input arguments for rpmem_create and rpmem_open
 */
static void
rpmem_log_args(const char *req, const char *target, const char *pool_set_name,
	void *pool_addr, size_t pool_size, unsigned nlanes)
{
	RPMEM_LOG(NOTICE, "%s request:", req);
	RPMEM_LOG(NOTICE, "\ttarget: %s", target);
	RPMEM_LOG(NOTICE, "\tpool set: %s", pool_set_name);
	RPMEM_LOG(INFO, "\tpool addr: %p", pool_addr);
	RPMEM_LOG(INFO, "\tpool size: %lu", pool_size);
	RPMEM_LOG(NOTICE, "\tnlanes: %u", nlanes);
}

/*
 * rpmem_log_resp -- log response attributes
 */
static void
rpmem_log_resp(const char *req, const struct rpmem_resp_attr *resp)
{
	RPMEM_LOG(NOTICE, "%s request response:", req);
	RPMEM_LOG(NOTICE, "\tnlanes: %u", resp->nlanes);
	RPMEM_LOG(NOTICE, "\tport: %u", resp->port);
	RPMEM_LOG(NOTICE, "\tpersist method: %s",
			rpmem_persist_method_to_str(resp->persist_method));
	RPMEM_LOG(NOTICE, "\tremote addr: 0x%" PRIx64, resp->raddr);
}

/*
 * rpmem_check_args -- validate user's arguments
 */
static int
rpmem_check_args(void *pool_addr, size_t pool_size, unsigned *nlanes)
{
	if (!pool_addr) {
		errno = EINVAL;
		ERR("invalid pool address");
		return -1;
	}

	if (!pool_size) {
		errno = EINVAL;
		ERR("invalid pool size");
		return -1;
	}

	if (!nlanes) {
		errno = EINVAL;
		ERR("lanes pointer cannot be NULL");
		return -1;
	}

	if (!(*nlanes)) {
		errno = EINVAL;
		ERR("number of lanes must be positive");
		return -1;
	}

	return 0;
}

/*
 * rpmem_create -- create remote pool on target node
 *
 * target        -- target node in format [<user>@]<target_name>[:<port>]
 * pool_set_name -- remote pool set name
 * pool_addr     -- local pool memory address which will be replicated
 * pool_size     -- required pool size
 * nlanes        -- number of lanes
 * create_attr   -- pool attributes used for creating the pool on remote node
 */
RPMEMpool *
rpmem_create(const char *target, const char *pool_set_name,
	void *pool_addr, size_t pool_size, unsigned *nlanes,
	const struct rpmem_pool_attr *create_attr)
{
	rpmem_log_args("create", target, pool_set_name,
			pool_addr, pool_size, *nlanes);

	if (rpmem_check_args(pool_addr, pool_size, nlanes))
		return NULL;

	RPMEMpool *rpp = rpmem_common_init(target);
	if (!rpp)
		goto err_common_init;

	struct rpmem_req_attr req = {
		.pool_size	= pool_size,
		.nlanes		= *nlanes,
		.provider	= rpp->provider,
		.pool_desc	= pool_set_name,
	};

	const char *remove_pool_set = NULL;
	struct rpmem_resp_attr resp;
	int ret = rpmem_obc_create(rpp->obc, &req, &resp, create_attr);
	if (ret) {
		ERR("!create request failed");
		goto err_obc_create;
	}

	remove_pool_set = pool_set_name;

	rpmem_log_resp("create", &resp);

	ret = rpmem_common_fip_init(rpp, &req, &resp,
			pool_addr, pool_size, nlanes);
	if (ret)
		goto err_fip_init;

	ret = pthread_create(&rpp->monitor, NULL, rpmem_monitor_thread, rpp);
	if (ret) {
		errno = ret;
		ERR("!starting monitor thread");
		goto err_monitor;
	}

	return rpp;
err_monitor:
	rpmem_common_fip_fini(rpp);
err_fip_init:
err_obc_create:
	rpmem_common_fini(rpp, 0, remove_pool_set);
err_common_init:
	return NULL;
}

/*
 * rpmem_open -- open remote pool on target node
 *
 * target        -- target node in format [<user>@]<target_name>[:<port>]
 * pool_set_name -- remote pool set name
 * pool_addr     -- local pool memory address which will be replicated
 * pool_size     -- required pool size
 * nlanes        -- number of lanes
 * open_attr     -- pool attributes, received from remote host
 */
RPMEMpool *
rpmem_open(const char *target, const char *pool_set_name,
	void *pool_addr, size_t pool_size, unsigned *nlanes,
	struct rpmem_pool_attr *open_attr)
{
	rpmem_log_args("open", target, pool_set_name,
			pool_addr, pool_size, *nlanes);

	if (rpmem_check_args(pool_addr, pool_size, nlanes))
		return NULL;

	RPMEMpool *rpp = rpmem_common_init(target);
	if (!rpp)
		goto err_common_init;

	struct rpmem_req_attr req = {
		.pool_size	= pool_size,
		.nlanes		= *nlanes,
		.provider	= rpp->provider,
		.pool_desc	= pool_set_name,
	};

	struct rpmem_resp_attr resp;

	int ret = rpmem_obc_open(rpp->obc, &req, &resp, open_attr);
	if (ret) {
		ERR("!open request failed");
		goto err_obc_create;
	}

	rpmem_log_resp("create", &resp);

	ret = rpmem_common_fip_init(rpp, &req, &resp,
			pool_addr, pool_size, nlanes);
	if (ret)
		goto err_fip_init;

	ret = pthread_create(&rpp->monitor, NULL, rpmem_monitor_thread, rpp);
	if (ret) {
		errno = ret;
		ERR("!starting monitor thread");
		goto err_monitor;
	}

	return rpp;
err_monitor:
	rpmem_common_fip_fini(rpp);
err_fip_init:
err_obc_create:
	rpmem_common_fini(rpp, 0, NULL);
err_common_init:
	return NULL;
}

/*
 * rpmem_close -- close remote pool on target node
 */
int
rpmem_close(RPMEMpool *rpp)
{
	RPMEM_LOG(INFO, "closing out-of-band connection");

	int ret = rpmem_obc_close(rpp->obc);
	if (ret)
		ERR("!close request failed");

	RPMEM_LOG(NOTICE, "out-of-band connection closed");

	rpmem_common_fip_fini(rpp);
	rpmem_common_fini(rpp, 1, NULL);

	return ret;
}

/*
 * rpmem_persist -- persist operation on target node
 *
 * rpp           -- remote pool handle
 * offset        -- offset in pool
 * length        -- length of persist operation
 * lane          -- lane number
 */
int
rpmem_persist(RPMEMpool *rpp, size_t offset, size_t length, unsigned lane)
{
	if (unlikely(rpp->error)) {
		errno = rpp->error;
		return -1;
	}

	int ret = rpmem_fip_persist(rpp->fip, offset, length, lane);
	if (unlikely(ret)) {
		ERR("persist operation failed");
		rpp->error = ret;
		errno = rpp->error;
		return -1;
	}

	return 0;
}

/*
 * rpmem_read -- read data from remote pool:
 *
 * rpp           -- remote pool handle
 * buff          -- output buffer
 * offset        -- offset in pool
 * length        -- length of read operation
 */
int
rpmem_read(RPMEMpool *rpp, void *buff, size_t offset, size_t length)
{
	if (unlikely(rpp->error)) {
		errno = rpp->error;
		return -1;
	}

	int ret = rpmem_fip_read(rpp->fip, buff, length, offset);
	if (unlikely(ret)) {
		rpp->error = ret;
		return -1;
	}

	return 0;
}
