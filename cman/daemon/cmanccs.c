/******************************************************************************
*******************************************************************************
**
**  Copyright (C) 2005-2006 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>
#include <syslog.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netdb.h>
#include <ifaddrs.h>

#include "logging.h"
#include "totemip.h"
#include "commands.h"
#include "ais.h"
#include "ccs.h"
#include "cnxman-socket.h"

#define CONFIG_VERSION_PATH	"/cluster/@config_version"
#define CLUSTER_NAME_PATH	"/cluster/@name"

#define EXP_VOTES_PATH		"/cluster/cman/@expected_votes"
#define TWO_NODE_PATH		"/cluster/cman/@two_node"
#define MCAST_ADDR_PATH		"/cluster/cman/multicast/@addr"
#define PORT_PATH		"/cluster/cman/@port"
#define KEY_PATH		"/cluster/cman/@keyfile"

#define NODEI_NAME_PATH		"/cluster/clusternodes/clusternode[%d]/@name"
#define NODE_NAME_PATH_BYNAME	"/cluster/clusternodes/clusternode[@name=\"%s\"]/@name"
#define NODE_NAME_PATH_BYNUM	"/cluster/clusternodes/clusternode[%d]/@name"
#define NODE_VOTES_PATH		"/cluster/clusternodes/clusternode[@name=\"%s\"]/@votes"
#define NODE_NODEID_PATH	"/cluster/clusternodes/clusternode[@name=\"%s\"]/@nodeid"
#define NODE_ALTNAMES_PATH	"/cluster/clusternodes/clusternode[@name=\"%s\"]/altname/@name"

#define MAX_NODENAMES 10
#define MAX_PATH_LEN PATH_MAX

/* Local vars - things we get from ccs */
static int nodeid;
static char *nodenames[MAX_NODENAMES];
static int num_nodenames;
static char *keyfile;
static char *mcast_name;
static struct cl_join_cluster_info join_info;

/* Get all the cluster node names from CCS and
 * add them to our node list.
 * Called when we start up and on "cman_tool version".
 */
int read_ccs_nodes()
{
    int ctree;
    char *nodename;
    char *str;
    int error;
    int i;
    int expected = 0;
    int config;

    /* Open the config file */
    ctree = ccs_force_connect(NULL, 1);
    if (ctree < 0)
    {
	    log_msg(LOG_ERR, "Error connecting to CCS");
	    return -1;
    }

    /* New config version */
    if (!ccs_get(ctree, CONFIG_VERSION_PATH, &str)) {
	    config = atoi(str);
	    free(str);

	    /* config_version is zero at startup when we read initial config */
	    if (config_version && config != config_version) {
		    ccs_disconnect(ctree);
		    log_msg(LOG_ERR, "CCS version is %d, we expected %d. config not updated\n",
			    config, config_version);
		    return -1;
	    }
	    config_version = config;
    }

    /* This overrides any other expected votes calculation /except/ for
       one specified on a join command-line */
    if (!ccs_get(ctree, EXP_VOTES_PATH, &str)) {
	    expected = atoi(str);
	    free(str);
    }

    if (!ccs_get(ctree, TWO_NODE_PATH, &str)) {
	    two_node = atoi(str);
	    free(str);
    }

    for (i=1;;i++)
    {
	char nodekey[256];
	char key[256];
	int  votes=0, nodeid=0;

	sprintf(nodekey, NODE_NAME_PATH_BYNUM, i);
	error = ccs_get(ctree, nodekey, &nodename);
	if (error)
	    break;

	sprintf(key, NODE_VOTES_PATH, nodename);
	if (!ccs_get(ctree, key, &str))
	{
	    votes = atoi(str);
	    free(str);
	} else
	    votes = 1;

	sprintf(key, NODE_NODEID_PATH, nodename);
	if (!ccs_get(ctree, key, &str))
	{
	    nodeid = atoi(str);
	    free(str);
	}

	P_MEMB("Got node %s from ccs (id=%d, votes=%d)\n", nodename, nodeid, votes);
	add_ccs_node(nodename, nodeid, votes, expected);

	free(nodename);
    }

    if (expected)
	    override_expected(expected);

    /* Finished with config file */
    ccs_disconnect(ctree);

    return 0;
}

static char *default_mcast(void)
{
        struct addrinfo *ainfo;
        struct addrinfo ahints;
	int ret;

        memset(&ahints, 0, sizeof(ahints));

        /* Lookup the the nodename address and use it's IP type to
	   default a multicast address */
        ret = getaddrinfo(nodenames[0], NULL, &ahints, &ainfo);
	if (ret) {
		log_msg(LOG_ERR, "Can't determine address family of nodename %s\n", nodenames[0]);
		return NULL;
	}

	if (ainfo->ai_family == AF_INET)
		return "239.192.9.1";
	if (ainfo->ai_family == AF_INET6)
		return "FF15::1";

	return NULL;
}

static int join(void)
{
	int error, i;
	int retlen;

	error = do_cmd_set_nodename(nodenames[0], &retlen);
	error = do_cmd_set_nodeid((char *)&nodeid, &retlen);

	/*
	 * Setup the interface/multicast
	 */
	for (i = 0; i<num_nodenames; i++)
	{
		error = ais_add_ifaddr(nodenames[i]);
		if (error)
			return error;
	}

	error = ais_set_mcast(mcast_name);
	if (error)
		return error;
        /*
	 * Join cluster
	 */
	error = do_cmd_join_cluster((char *)&join_info, &retlen);
	if (error)
		return error;

	return 0;
}


static int verify_nodename(int cd, char *nodename)
{
	char path[MAX_PATH_LEN];
	char nodename2[MAX_CLUSTER_MEMBER_NAME_LEN+1];
	char nodename3[MAX_CLUSTER_MEMBER_NAME_LEN+1];
	char *str, *dot = NULL;
	struct ifaddrs *ifa, *ifa_list;
	struct sockaddr *sa;
	int error, i;


	/* nodename is either from commandline or from uname */

	str = NULL;
	memset(path, 0, MAX_PATH_LEN);
	sprintf(path, NODE_NAME_PATH_BYNAME, nodename);

	error = ccs_get(cd, path, &str);
	if (!error) {
		free(str);
		return 0;
	}

	/* if nodename was from uname, try a domain-less version of it */

	strcpy(nodename2, nodename);
	dot = strstr(nodename2, ".");
	if (dot) {
		*dot = '\0';

		str = NULL;
		memset(path, 0, MAX_PATH_LEN);
		sprintf(path, NODE_NAME_PATH_BYNAME, nodename2);

		error = ccs_get(cd, path, &str);
		if (!error) {
			free(str);
			strcpy(nodename, nodename2);
			return 0;
		}
	}


	/* if nodename (from uname) is domain-less, try to match against
	   cluster.conf names which may have domainname specified */

	for (i = 1; ; i++) {
		str = NULL;
		memset(path, 0, 256);
		sprintf(path, "/cluster/clusternodes/clusternode[%d]/@name", i);

		error = ccs_get(cd, path, &str);
		if (error || !str)
			break;

		strcpy(nodename3, str);
		dot = strstr(nodename3, ".");
		if (dot)
			*dot = '\0';

		if (strlen(nodename2) == strlen(nodename3) &&
		    !strncmp(nodename2, nodename3, strlen(nodename3))) {
			free(str);
			strcpy(nodename, nodename3);
			return 0;
		}

		free(str);
	}


	/* the cluster.conf names may not be related to uname at all,
	   they may match a hostname on some network interface */

	error = getifaddrs(&ifa_list);
	if (error)
		return -1;

	for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
		sa = ifa->ifa_addr;
		if (!sa || sa->sa_family != AF_INET)
			continue;

		error = getnameinfo(sa, sizeof(*sa), nodename2,
				    sizeof(nodename2), NULL, 0, 0);
		if (error)
			goto out;

		str = NULL;
		memset(path, 0, 256);
		sprintf(path, NODE_NAME_PATH_BYNAME, nodename2);

		error = ccs_get(cd, path, &str);
		if (!error) {
			free(str);
			strcpy(nodename, nodename2);
			goto out;
		}

		/* truncate this name and try again */

		dot = strstr(nodename2, ".");
		if (!dot)
			continue;
		*dot = '\0';

		str = NULL;
		memset(path, 0, 256);
		sprintf(path, NODE_NAME_PATH_BYNAME, nodename2);

		error = ccs_get(cd, path, &str);
		if (!error) {
			free(str);
			strcpy(nodename, nodename2);
			goto out;
		}
	}

	error = -1;
 out:
	freeifaddrs(ifa_list);
	return error;
}


static int get_ccs_join_info(void)
{
	char path[MAX_PATH_LEN];
	char nodename[MAX_CLUSTER_MEMBER_NAME_LEN+1];
	char *str, *name, *cname = NULL;
	int cd, error, i, vote_sum = 0, node_count = 0;

	/* connect to ccsd */

	if (getenv("CMAN_CLUSTER_NAME")) {
		cname = getenv("CMAN_CLUSTER_NAME");
		log_msg(LOG_INFO, "Using override cluster name %s\n", cname);
	}

	cd = ccs_force_connect(cname, 1);
	if (cd < 0) {
		log_msg(LOG_ERR, "Error connecting to CCS");
		return -ENOTCONN;
	}

	/* cluster name */

	error = ccs_get(cd, CLUSTER_NAME_PATH, &str);
	if (error) {
		log_msg(LOG_ERR, "cannot find cluster name in config file");
		return -ENOENT;
	}

	if (cname) {
		if (strcmp(cname, str)) {
			log_msg(LOG_ERR, "cluster names not equal %s %s", cname, str);
			return -ENOENT;
		}
	} else {
		strcpy(join_info.cluster_name, str);
	}
	free(str);


	/* our nodename */

	memset(nodename, 0, sizeof(nodename));

	if (getenv("CMAN_NODENAME")) {
		strcpy(nodename, getenv("CMAN_NODENAME"));
		log_msg(LOG_INFO, "Using override node name %s\n", nodename);
	}
	else {
		struct utsname utsname;
		error = uname(&utsname);
		if (error) {
			log_msg(LOG_ERR, "cannot get node name, uname failed");
			return -ENOENT;
		}
		strcpy(nodename, utsname.nodename);
	}


	/* find our nodename in cluster.conf */

	error = verify_nodename(cd, nodename);
	if (error) {
		log_msg(LOG_ERR, "local node name \"%s\" not found in cluster.conf",
			nodename);
		return -ENOENT;
	}

	join_info.expected_votes = 0;
	if (getenv("CMAN_EXPECTEDVOTES")) {
		join_info.expected_votes = atoi(getenv("CMAN_EXPECTEDVOTES"));
		if (join_info.expected_votes < 1) {
			log_msg(LOG_ERR, "CMAN_EXPECTEDVOTES environment variable is invalid, ignoring");
			join_info.expected_votes = 0;
		}
		else {
			log_msg(LOG_INFO, "Using override expected votes %d\n", join_info.expected_votes);
		}
	}

	/* sum node votes for expected */
	if (join_info.expected_votes == 0) {
		for (i = 1; ; i++) {
			name = NULL;
			memset(path, 0, MAX_PATH_LEN);
			sprintf(path, NODEI_NAME_PATH, i);

			error = ccs_get(cd, path, &name);
			if (error || !name)
				break;

			node_count++;

			memset(path, 0, MAX_PATH_LEN);
			sprintf(path, NODE_VOTES_PATH, name);
			free(name);

			error = ccs_get(cd, path, &str);
			if (error)
				vote_sum++;
			else {
				if (atoi(str) < 0) {
					log_msg(LOG_ERR, "negative votes not allowed");
					return -EINVAL;
				}
				vote_sum += atoi(str);
				free(str);
			}
		}

		/* optional expected_votes supercedes vote sum */

		error = ccs_get(cd, EXP_VOTES_PATH, &str);
		if (!error) {
			join_info.expected_votes = atoi(str);
			free(str);
		} else
			join_info.expected_votes = vote_sum;
	}

	/* optional port */
	if (getenv("CMAN_IP_PORT")) {
		join_info.port = atoi(getenv("CMAN_IP_PORT"));
		log_msg(LOG_INFO, "Using override IP port %d\n", join_info.port);
	}

	if (!join_info.port) {
		error = ccs_get(cd, PORT_PATH, &str);
		if (!error) {
			join_info.port = atoi(str);
			free(str);
		}
		else
			join_info.port = 6809;
	}

	/* optional security key filename */

	error = ccs_get(cd, KEY_PATH, &str);
	if (!error) {
		keyfile = str;
	}


	/* find our own number of votes */
	if (getenv("CMAN_VOTES")) {
		join_info.votes = atoi(getenv("CMAN_VOTES"));
		log_msg(LOG_INFO, "Using override votes %d\n", join_info.votes);
	}

	if (!join_info.votes) {
		memset(path, 0, MAX_PATH_LEN);
		sprintf(path, NODE_VOTES_PATH, nodename);

		error = ccs_get(cd, path, &str);
		if (!error) {
			int votes = atoi(str);
			if (votes < 0 || votes > 255) {
				log_msg(LOG_ERR, "invalid votes value %d", votes);
				return -EINVAL;
			}
			join_info.votes = votes;
			free(str);
		}
		else {
			join_info.votes = 1;
		}
	}


	/* nodeid */
	if (getenv("CMAN_NODEID")) {
		nodeid = atoi(getenv("CMAN_NODEID"));
		log_msg(LOG_INFO, "Using override nodeid %d\n", nodeid);
	}

	if (!nodeid) {
		memset(path, 0, MAX_PATH_LEN);
		sprintf(path, NODE_NODEID_PATH, nodename);

		error = ccs_get(cd, path, &str);
		if (!error) {
			nodeid = atoi(str);
			free(str);
		}
	}

	/* get all alternative node names */

	nodenames[0] = strdup(nodename);
	num_nodenames = 1;

	memset(path, 0, MAX_PATH_LEN);
	sprintf(path, NODE_ALTNAMES_PATH, nodename);

	for (i = 1; ; i++) {
		str = NULL;

		error = ccs_get(cd, path, &str);
		if (error || !str)
			break;

		/* If we get the same thing twice, it's probably the
		   end of a 1-element list */

		if (strcmp(str, nodenames[i-1]) == 0) {
			free(str);
			break;
		}

		nodenames[i] = str;
		num_nodenames++;
	}

	if (getenv("CMAN_MCAST_ADDR")) {
		mcast_name = getenv("CMAN_MCAST_ADDR");
		log_msg(LOG_INFO, "Using override multicast address %s\n", mcast_name);
	}

	/* optional multicast name */
	if (!mcast_name) {
		error = ccs_get(cd, MCAST_ADDR_PATH, &str);
		if (!error) {
			mcast_name = str;
		}
	}

	if (!mcast_name) {
		mcast_name = default_mcast();
		log_msg(LOG_INFO, "Using default multicast address of %s\n", mcast_name);
	}

	/* two_node mode */

	error = ccs_get(cd, TWO_NODE_PATH, &str);
	if (!error) {
		join_info.two_node = atoi(str);
		free(str);
		if (join_info.two_node) {
			if (node_count != 2 || vote_sum != 2) {
				log_msg(LOG_ERR, "the two-node option requires exactly two "
					"nodes with one vote each and expected "
					"votes of 1 (node_count=%d vote_sum=%d)",
					node_count, vote_sum);
				return -EINVAL;
			}

			if (join_info.votes != 1) {
				log_msg(LOG_ERR, "the two-node option requires exactly two "
					"nodes with one vote each and expected "
					"votes of 1 (votes=%d)", join_info.votes);
				return -EINVAL;
			}
		}
	}

	ccs_disconnect(cd);
	return 0;
}



/* Read just the stuff we need to get started.
   This does what 'cman_tool join' used to to */
int read_ccs_config()
{
	int error;

	error = get_ccs_join_info();
	if (error) {
		log_msg(LOG_ERR, "Error reading CCS info, cannot start");
		return error;
	}

	error = join();

	return error;
}
