deps/redis: deps/redis/src/redismodule.h
deps/redis/src/redismodule.h:
	git submodule update --init -- deps/redis

deps/zstd: deps/libzstd/lib/libzstd.a
deps/libzstd/lib/libzstd.a:
	git submodule update --init -- deps/zstd
	cd deps/zstd && make

.PHONY: deps/redis deps/zstd
