.DEFAULT_GOAL := alaska

MAKEFLAGS += --no-print-directory



ROOT=$(shell pwd)
export PATH:=$(ROOT)/local/bin:$(PATH)
export LD_LIBRARY_PATH:=$(ROOT)/local/lib:$(LD_LIBRARY_PATH)

CC=clang
CXX=clang++
export CC
export CXX

BUILD=build


BUILD_REQ=$(BUILD)/Makefile

$(BUILD)/Makefile:
	@mkdir -p $(BUILD)
	@cd $(BUILD) && cmake ../ -DCMAKE_INSTALL_PREFIX:PATH=$(ROOT)/local

alaska: $(BUILD_REQ)
	@$(MAKE) -C $(BUILD) install
	@cp build/compile_commands.json .

sanity: alaska
	@local/bin/alaska -O3 test/sanity.c -o build/sanity
	@build/sanity

test: alaska
	@build/runtime/alaska_test

.PHONY: alaska all

# Run compilation unit tests to validate that the compiler can
# handle all the funky control flow in the GCC test suite
unit: alaska FORCE
	tools/unittest.py

docs:
	@doxygen Doxyfile


# Defer to CMake to clean itself, if the build folder exists
clean:
	[ -d $(BUILD) ] && make -C $(BUILD) clean
	rm -f .*.o*

mrproper:
	rm -rf $(BUILD) .*.o*


docker:
	docker build -t alaska .
	docker run -it --rm alaska bash

deps: local/bin/gclang local/bin/clang

local/bin/gclang:
	tools/build_gclang.sh

local/bin/clang:
	tools/get_llvm.sh


redis: FORCE
	nix develop --command bash -c "source enable && make -C test/redis"



# Build memcached w/ anchorage
memcached/bin/memcached-alaska:
	@ . opt/enable-alaska-anchorage \
		&& $(MAKE) -C memcached memcached

# Build redis w/ anchorage
redis/bin/redis-server-alaska: venv opt/enable-alaska-anchorage
	@. venv/bin/activate && \
		. opt/enable-alaska-anchorage \
		&& $(MAKE) -C redis redis

redis/bin/redis-server-ad: venv opt/enable-alaska-anchorage
	@. venv/bin/activate && \
		. opt/enable-alaska-anchorage \
		&& $(MAKE) -C redis redis




# Create the results for redis
results/figure9.pdf: venv

	@echo "Compiling redis"
	@. opt/enable-alaska-anchorage \
		&& make -C redis redis

	@echo "Generating data for figure 9"
	@mkdir -p results
	@. venv/bin/activate \
		&& . opt/enable-alaska-anchorage \
		&& ulimit -s unlimited \
		&& python3 -m redis.frag \
		&& python3 plotgen/figure9.py




results/figure10.pdf: venv | results/figure9.pdf
	@echo "Generating data for figure 10"
	@mkdir -p results
	@. venv/bin/activate \
		&& . opt/enable-alaska-anchorage \
		&& ulimit -s unlimited \
		&& python3 -m redis.config_sweep \
		&& python3 plotgen/figure10.py



# Create the results for redis (the large version)
results/redis-alaska-large.csv: venv redis/bin/redis-server-alaska
	@echo "Generating data for figure 11"
	@mkdir -p results
	@. venv/bin/activate \
	   && . opt/enable-alaska-anchorage \
		 && ulimit -s unlimited \
		 && python3 -m redis.frag_large

results/figure11.pdf: venv results/redis-alaska-large.csv
	@echo "Plotting figure 11"
	@. venv/bin/activate \
		&& python3 plotgen/figure9.py




results/memcached-sweep.csv: venv memcached/bin/memcached-alaska
	@echo "Generating data for figure 12"
	@mkdir -p results
	@. venv/bin/activate \
	   && . opt/enable-alaska-anchorage \
		 && ulimit -s unlimited \
		 && python3 -m memcached.ycsb

results/figure12.pdf: venv results/memcached-sweep.csv
	@echo "Plotting figure 12"
	@. venv/bin/activate \
		&& python3 plotgen/figure12.py



compile:
	@./build.sh



distclean:
	make -C redis clean
	rm -rf opt build bench venv results

# If these files don't exist, we need to compile.
opt/enable-alaska-noservice: compile
opt/enable-alaska-anchorage: compile

in-docker: FORCE
	docker build -t alaska-asplos24ae .
	docker run -it --rm --mount type=bind,source=${PWD},target=/artifact alaska-asplos24ae

in-podman: FORCE
	podman build -t alaska-asplos24ae .
	podman run -it --rm --mount type=bind,source=${PWD},target=/artifact alaska-asplos24ae

FORCE:
