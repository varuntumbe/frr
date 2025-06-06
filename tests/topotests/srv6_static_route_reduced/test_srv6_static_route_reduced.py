#!/usr/bin/env python
# SPDX-License-Identifier: ISC

#
# test_srv6_static_route_reduced.py
#
# Copyright 2025
# Carmine Scarpitta <cscarpit.@cisco.com>
#

"""
test_srv6_static_route_reduced.py:
Test for SRv6 static route on zebra
"""

import os
import sys
import json
import pytest
import functools

CWD = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(CWD, "../"))

# pylint: disable=C0413
from lib import topotest
from lib.topogen import Topogen, get_topogen
from lib.topolog import logger
from lib.common_config import required_linux_kernel_version

pytestmark = [pytest.mark.staticd]


def open_json_file(filename):
    try:
        with open(filename, "r") as f:
            return json.load(f)
    except IOError:
        assert False, "Could not read file {}".format(filename)


def setup_module(mod):
    result = required_linux_kernel_version("6.0")
    if result is not True:
        pytest.skip("Kernel requirements are not met, kernel version should be >=6.0")

    tgen = Topogen({None: "r1"}, mod.__name__)
    tgen.start_topology()
    for rname, router in tgen.routers().items():
        router.run("/bin/bash {}/{}/setup.sh".format(CWD, rname))
        router.load_frr_config("frr.conf")
    tgen.start_router()


def teardown_module():
    tgen = get_topogen()
    tgen.stop_topology()


def test_srv6_static_route():
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)
    router = tgen.gears["r1"]

    def _check_rib(router, expected_route_file):
        logger.info("checking Zebra RIB")
        output = json.loads(router.vtysh_cmd("show ip route static json"))
        expected = open_json_file("{}/{}".format(CWD, expected_route_file))
        return topotest.json_cmp(output, expected)

    def check_rib(router, expected_file):
        func = functools.partial(_check_rib, router, expected_file)
        _, result = topotest.run_and_expect(func, None, count=20, wait=3)
        assert result is None, "Failed"

    def _check_rib_v6(router, expected_route_file):
        logger.info("checking Zebra RIB")
        output = json.loads(router.vtysh_cmd("show ipv6 route static json"))
        expected = open_json_file("{}/{}".format(CWD, expected_route_file))
        return topotest.json_cmp(output, expected)

    def check_rib_v6(router, expected_file):
        func = functools.partial(_check_rib_v6, router, expected_file)
        _, result = topotest.run_and_expect(func, None, count=20, wait=3)
        assert result is None, "Failed"

    # FOR DEVELOPER:
    # If you want to stop some specific line and start interactive shell,
    # please use tgen.mininet_cli() to start it.

    logger.info("Test for SRv6 route configuration")
    check_rib(router, "r1/show_ip_route.json")
    check_rib_v6(router, "r1/show_ipv6_route.json")


if __name__ == "__main__":
    args = ["-s"] + sys.argv[1:]
    sys.exit(pytest.main(args))
