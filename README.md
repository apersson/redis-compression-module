# Transparent compression module for Redis

The module provides:

- Zstandard compression
- Optional transparent mode for Strings where SET/GET commands are translated
  to the equivalant compress / decompress command
  ([Example](#enable-transparent-mode)).
- Ability to train Zstandard dictionaries on data stored in Redis
  ([Example](#train-a-default-dictionary)).
- Prefix-specific dictionary support: different keys can use different
  dictionaries to maximize compression efficiency
  ([Example](#train-a-prefix-specific-dictionary)).

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

### Working with Dictionaries

**WARNING**: Traning a dictionary leaks memory (~6 MB per operation). It's
recommended that all TRAIN operations are performed on a replica.

#### Train a Default Dictionary

A default dictionary will be used for any key that doesn't have prefix that
matches a prefix-specific dictionary []. To train a new default dictionary:
```
$ redis-cli compress.dict train
1) (integer) 1600643999514
2) (integer) 21748
3) (integer) 397
```

In the above example, the dictionary has ID 1600643999514 and is 21748 bytes.
It was trained on 397 objects. The default target size for dictionaries is
100 KB, but can be changed using the `DICTSIZE` option.


#### Train a Prefix Specific Dictionary

To train a new dictionary for keys that match a specific prefix, use the
`PREFIX` option:
```
$ redis-cli compress.dict train prefix foo
```

You can use the [`COMPRESS.DICT LIST`](#compressdict-list) command to get
details about loaded dictionaries.

## Commands
### COMPRESS.SET key value
Compresses value and stores it in key. If a key already holds a value, it's
overwritten and TTL is discarded. If data cannot be compressed, then key is
turned into a normal (uncompressed) string.

> **_NOTE_** `COMPRESS.SET` does not currently support the standard `SET`
options.

#### Returns
Simple string, or Null reply. Same as the standard
[`SET`](https://redis.io/commands/set) command.

### COMPRESS.GET key
Returns the decompressed version of key.

#### Returns
Bulk string. Value of key, or nil when the key does not exists.

### COMPRESS.TRANSPARENT on|off
Toggle transparent compression mode. When transparent mode is ON:
 - `SET` operations are transformed to `COMPRESS.SET`.
 - `GET` operations are transformed to `COMPRESS.GET`

#### Returns
Integer reply: number of command filters enabled or disabled.

#### Example
```
redis> COMPRESS.TRANSPARENT on
2
```

### COMPRESS.DICT TRAIN [DICTSIZE size] [PREFIX prefix]
Train a new dictionary using data stored in Redis.

> **_WARNING_** Training a dictionary leaks about 6 MB of memory and the
operation runs synchronously and can therefor block other operations for
a significant period of time. **It is strongly recommended that all TRAIN
operations are preformed on a replica.**

#### Returns
An array with the following items:

 - Dictionary ID
 - Dictionary size in bytes
 - Number of objects used to trains the dictionary

### COMPRESS.DICT DROP dictID

Removes the dictionary so that no new objects can be compressed using it.
However, the dictionary will only be removed from Redis once all objects
already compressed using the dictionary are removed.

#### Returns
Simple string.

### COMPRESS.DICT DUMP
Dumps the content of a dictionary so that it can later be loaded using
[`COMPRESS.DICT RESTORE`](#compressdict-restore).

#### Returns
Bulk string.

#### Example
```
$ redis-cli compress.dict dump 1600643999514 > dict.raw
$ cat dict.raw | redis-cli -x compress.dict restore
```

### COMPRESS.DICT RESTORE <dictBuffer>
Creates a new dictionary from the provided dictionary data. The data could have
been obtained using [`COMPRESS.DICT DUMP`](#compressdict-dump) or
through any `zstd --train`.

> **NOTE** Currently only supported for the default dictionary.

#### Returns
Simple string.

### COMPRESS.DICT LIST

List available dictionaries.

#### Returns
An array of arrays, where each sub-array represents a dictionary. Each
dictionary has the following:

 - Dictionary ID
 - Prefix (or "" if none)
 - Number of objects currently compressed using the dictionary
 - Uncompressed size of all objects using the dictionary
 - Compressed size of all objects using the dictionary
 - Compression ratio

#### Example
```
redis> COMPRESS.DICT LIST
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
