# Copyright 2017-2019 Authors of Cilium
# SPDX-License-Identifier: Apache-2.0

include ../Makefile.defs

provision = true
# If you set provision to false the test will run without bootstrapping the
# cluster again.

TEST_ARTIFACTS = ./tmp.yaml ./*_service_manifest.json ./*_manifest.yaml
TEST_ARTIFACTS += ./*_policy.json ./k8s-*.xml ./runtime.xml ./test_results
TEST_ARTIFACTS += ./test.test

GINKGO = $(QUIET) ginkgo

all: build

build:
	# This should print a message if ginkgo is missing, but allow the
	# bash-based test VM to run CI without needing to add Ginkgo to it.
	# Once the bash-based tests are migrated, the "|| true" can be removed.
	# GH #1839
	@$(ECHO_GINKGO)$@
	$(GINKGO) build || true
	$(QUIET)$(MAKE) -C bpf/

test: run k8s

run:
	ginkgo --focus "Runtime" -v -- --cilium.provision=$(provision)

k8s:
	ginkgo --focus "K8s" -v -- --cilium.provision=$(provision)

nightly:
	ginkgo --focus "Nightly" -v -- --cilium.provision=$(provision)

clean:
	@$(ECHO_CLEAN)
	-$(QUIET) rm -rf $(TEST_ARTIFACTS)
	-$(QUIET) rm -f .vagrant/*.box
