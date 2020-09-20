# Transparent compression module for Redis

The module provides:

- Zstandard compression
- Transparent compression/decompression of STRING objects
  (COMPRESS.TRANSPARENT on|off)
- Support for Zstandard dictionaries, with optional prefix-specific
  dictionaries

## Basic Usage

```
$ redis-server --loadmodule librediscompress.so
$ cat blob | redis-cli -x compress.set foo
$ redis-cli compress.get foo
```

Check effectivness of compression

```
$ redis-cli info modules
...
# Modules
module:name=compress,ver=1,api=1,filters=0,usedby=[],using=[],options=[]

# compress_stats
compress_zstd_version:10405
compress_compression_ratio:7.70
compress_compressed_size:210842569
compress_uncompressed_size:1623962898
compress_objects:100000
compress_dictionaries:23
```

## Advanced

### Enable Transparent Mode

```
$ redis-cli compress.transparent on
$ cat blob | redis-cli -x set foo
$ redis-cli type foo
ZipStr001
$ redis-cli get foo
```

## Working with Dictionaries

WARNING: Traning a dictionary leaks memory (~6 MB per operation). It's
         recommended that all TRAIN operations are performed on a replica.

Train a new default dictionary:
```
$ redis-cli compress.dict train
1) (integer) 1600643999514
2) (integer) 21748
3) (integer) 397
```

In the above example, the dictionary has ID 1600643999514 and is 21748 bytes.
It was trained on 397 objects.

Train a new dictionary for keys that match a specific prefix:
```
$ redis-cli compress.dict train prefix foo
```

Use `COMPRESS.DICT LIST` to get details about available dictionaries. The
command returns an array for each dictionary, with the following information:

 - Dictionary ID
 - Prefix (or "" if none)
 - Objects compressed using the dictionary
 - Uncompressed size of all objects using the dictionary
 - Compressed size of all objects using the dictionary
 - Compression ratio

For example:

```
1) 1) (integer) 1600644598087
   2) "foo"
   3) (integer) 398
   4) (integer) 39437
   5) (integer) 17635
   6) "2.2362914658349871"
2) 1) (integer) 1600644598117
   2) "bar"
   3) (integer) 72
   4) (integer) 28946
   5) (integer) 22003
   6) "1.3155478798345681"
```

The module currently assumes that colon follows the prefix, i.e., keys should follow
the format PREFIX:key.

Use `COMPRESS.DICT HELP` to list all dictionary operations.
