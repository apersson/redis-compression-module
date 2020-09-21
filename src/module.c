#include <string.h>
#include <strings.h>
#include <stdint.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "deps/zstd/lib/zstd.h"
#include "deps/zstd/lib/common/zstd_errors.h"
#define ZDICT_STATIC_LINKING_ONLY
#include "deps/zstd/lib/dictBuilder/zdict.h"

#define REDISMODULE_EXPERIMENTAL_API
#include "deps/redis/src/redismodule.h"



#define	MODPREFIX	"compress"
#define BUFSIZE		10*1024*1024

#define	ZIPSTR_ENCODING_VERSION	0

struct dict {
	unsigned long refcnt;

	long long id;
	char *prefix;
	size_t prefix_len;
	size_t mem_uncompressed;
	size_t mem_compressed;

	ZSTD_CDict *cdict;
	ZSTD_DDict *ddict;
	char *buf;
	size_t buflen;
};

struct compress_module {
	char *buf;			/* tmp buffer for compress/uncompress */
	size_t buflen;
	int clevel;			/* Compression level */

	struct dict *dict;		/* Default dictionary */

	RedisModuleDict *all_dicts;	/* All dictionaries */
	RedisModuleDict *prefix_dicts;	/* Active prefix dictionaries */

	RedisModuleString *zstd_set_str;
	RedisModuleCommandFilter *set_filter;

	RedisModuleString *zstd_get_str;
	RedisModuleCommandFilter *get_filter;

	ZSTD_CCtx *cctx;
	ZSTD_DCtx *dctx;

	size_t mem_total_uncompressed;
	size_t mem_total_compressed;
	size_t nobjs;
};

/*
 * Compressessed String object.
 */
struct zipstr {
	size_t orig_len;
	size_t len;
	struct dict *dict;
	char buf[];
};

struct train_data {
	const char *match_prefix;
	size_t match_len;
	char *buf;
	size_t buflen;
	size_t offset;
	size_t nsamples;
	size_t max_nsamples;
	size_t *sample_sizes;
};

static RedisModuleType *ZipString_Type;
static struct compress_module module;


void dict_hold(struct dict *dict, const struct zipstr *zs) {
	if (dict == NULL)
		return;

	dict->refcnt++;

	if (zs != NULL) {
		dict->mem_uncompressed += zs->orig_len;
		dict->mem_compressed += zs->len;
	}
}

void dict_free(struct dict *dict) {
	if (dict->cdict != NULL)
		ZSTD_freeCDict(dict->cdict);
	if (dict->ddict != NULL)
		ZSTD_freeDDict(dict->ddict);
	RedisModule_Free(dict->buf);
	RedisModule_Free(dict);
}

void dict_rele(struct compress_module *mod, struct dict *dict,
    const struct zipstr *zs) {
	if (dict == NULL)
		return;

	if (--dict->refcnt == 0) {
		RedisModule_DictDelC(mod->all_dicts, &dict->id,
		    sizeof (dict->id), NULL);
		dict_free(dict);
		return;
	}

	if (zs != NULL) {
		dict->mem_uncompressed -= zs->orig_len;
		dict->mem_compressed -= zs->len;
	}
}

/*
 * Create ref counted dictionary.
 */
long long dict_create_with_id(struct compress_module *mod, long long id,
    const char *buf, size_t buflen, const char *prefix, size_t prefix_len,
    int clevel) {

	struct dict *const dict = RedisModule_Alloc(sizeof (*dict));

	dict->id = id;
	dict->prefix = NULL;
	dict->prefix_len = 0;
	if (prefix_len > 0) {
		dict->prefix = RedisModule_Alloc(prefix_len);
		(void) memcpy(dict->prefix, prefix, prefix_len);
		dict->prefix_len = prefix_len;
	}
	dict->mem_uncompressed = 0;
	dict->mem_compressed = 0;

	dict->buflen = buflen;
	dict->buf = RedisModule_Alloc(dict->buflen);
	(void) memcpy(dict->buf, buf, dict->buflen);
	dict->cdict = ZSTD_createCDict_byReference(dict->buf, buflen, clevel);
	dict->ddict = ZSTD_createDDict_byReference(dict->buf, buflen);

	if (dict->cdict == NULL || dict->ddict == NULL) {
		RedisModule_Log(NULL, "error", "Could not create dict");
		dict_free(dict);
		return -1;
	}
	dict->refcnt = 1;

	/* drop previous dictionary */
	dict_rele(mod, mod->dict, NULL);

	/* XXX */
	(void) RedisModule_DictSetC(mod->all_dicts, &dict->id,
	    sizeof (dict->id), dict);

	if (prefix != NULL) {
		/* TODO check for an existing dict, mark as inactive */
		(void) RedisModule_DictSetC(mod->prefix_dicts, dict->prefix,
		    dict->prefix_len, dict);
	} else {
		/* Default dict */
		mod->dict = dict;
	}


	RedisModule_Log(NULL, "error", "ALL OK");
	return dict->id;
}

long long dict_create(struct compress_module *mod, const char *buf,
    size_t buflen, const char *prefix, size_t prefix_len, int clevel) {

	return dict_create_with_id(mod, RedisModule_Milliseconds(),
	    buf, buflen, prefix, prefix_len, clevel);
}

void zipstr_free(void *value) {
	struct zipstr *const zs = value;

	module.mem_total_uncompressed -= zs->orig_len;
	module.mem_total_compressed -= zs->len;
	module.nobjs--;

	dict_rele(&module, zs->dict, zs);

	RedisModule_Free(zs);
}

struct zipstr *zipstr_alloc(struct compress_module *module,
    struct dict *dict, const char *compressed_data, size_t len,
    size_t orig_len) {

	/* Data was compressed successfully; create an object */ 
	struct zipstr *const zs = RedisModule_Alloc(sizeof(*zs) + len);
	zs->orig_len = orig_len;
	zs->len = len;
	zs->dict = dict;
	(void) memcpy(zs->buf, compressed_data, zs->len);

	dict_hold(zs->dict, zs);
	
	module->mem_total_uncompressed += zs->orig_len;
	module->mem_total_compressed += zs->len;
	module->nobjs++;

	return zs;
}

/*
 * Create a compressed string from the original data.
 */
struct zipstr *zipstr_create(struct compress_module *module, const char *key,
    size_t keylen, const char *data, size_t len) {

	const char *const pos = memchr(key, ':', keylen);
	struct dict *dict = NULL;
	size_t clen;

	if (pos != NULL) {
		/* look for dict */
		dict = RedisModule_DictGetC(module->prefix_dicts,
		    (void *)key, (pos - key), NULL);
	}
	if (dict == NULL) {
		dict = module->dict;
	}

	/* Use dictionary, if available */
	if (dict != NULL) {
		clen = ZSTD_compress_usingCDict(module->cctx, module->buf,
		    module->buflen, data, len, dict->cdict);
	} else {
		clen = ZSTD_compress(module->buf, module->buflen, data, len,
		    module->clevel);
	}

	if (ZSTD_isError(clen) != 0) {
		return NULL;
	}

	/* Data was compressed successfully; allocate the object */ 
	return zipstr_alloc(module, dict, module->buf, clen, len);
}

void zipstr_rdb_save(RedisModuleIO *rdb, void *value) {
	struct zipstr *const zs = value;
	uint64_t dict_id = 0;

	if (zs->dict != NULL) {
		dict_id = zs->dict->id;
	}
	RedisModule_SaveUnsigned(rdb, zs->orig_len); 
	// TODO No need to encode the length; it's in the RDB.
	RedisModule_SaveUnsigned(rdb, zs->len); 
	RedisModule_SaveUnsigned(rdb, dict_id); 
	RedisModule_SaveStringBuffer(rdb, zs->buf, zs->len);
}

void *zipstr_rdb_load(RedisModuleIO *rdb, int encver) {
	if (encver != ZIPSTR_ENCODING_VERSION) {
		RedisModule_Log(NULL, "notice", "Unknown version (%d)", encver);
		return NULL;
	}

	const uint64_t orig_len = RedisModule_LoadUnsigned(rdb);
	const uint64_t len = RedisModule_LoadUnsigned(rdb);
	const uint64_t dict_id = RedisModule_LoadUnsigned(rdb);
	char *buf = RedisModule_LoadStringBuffer(rdb, NULL);
	struct dict *dict = NULL;

	if (dict_id != 0) {
		/* TODO if dict_id != 0, lookup dict */
		dict = RedisModule_DictGetC(module.all_dicts,
		    (void *)&dict_id, sizeof (dict_id), NULL);
		if (dict == NULL) {
			RedisModule_Log(NULL, "error",
			    "Could not find dict (%llu) for object",
			    dict_id);
			RedisModule_Free(buf);
			return NULL;
		}
		RedisModule_Log(NULL, "debug", "Found dict (%llu) for object",
		    dict_id);
	}

	struct zipstr *const zs = zipstr_alloc(&module, dict, buf, len,
	    orig_len);
	RedisModule_Free(buf);

	return zs;
}

void zipstr_aux_save(RedisModuleIO *rdb, int when) {
	RedisModule_Log(NULL, "error", "AUX Save");

	/* Store dictionary information before the RDB data */
	if (when != REDISMODULE_AUX_BEFORE_RDB) {
		return;
	}

	const uint64_t ndicts = RedisModule_DictSize(module.all_dicts);
	RedisModule_Log(NULL, "debug", "Saving %llu dicts", ndicts);
	RedisModule_SaveUnsigned(rdb, ndicts);

	RedisModuleDictIter *const iter = RedisModule_DictIteratorStartC(
	    module.all_dicts, "^", NULL, 0);

	void *data;
	while (RedisModule_DictNextC(iter, NULL, &data) != NULL) {
		const struct dict *dict = data;

		RedisModule_Log(NULL, "debug", "Saving dict %llu", dict->id);
		RedisModule_SaveUnsigned(rdb, dict->id);
		RedisModule_SaveUnsigned(rdb, dict->prefix_len);
		if (dict->prefix_len > 0) {
			RedisModule_SaveStringBuffer(rdb, dict->prefix,
			    dict->prefix_len);
		}
		RedisModule_SaveStringBuffer(rdb, dict->buf, dict->buflen);
	}

	RedisModule_DictIteratorStop(iter);
}

int zipstr_aux_load(RedisModuleIO *rdb, int encver, int when) {
	RedisModule_Log(NULL, "error", "AUX Load");

	if (encver != ZIPSTR_ENCODING_VERSION) {
		RedisModule_Log(NULL, "warning",
		    "Unknown encoding version (%d)", encver);
		return REDISMODULE_ERR;
	}

	if (when != REDISMODULE_AUX_BEFORE_RDB) {
		return REDISMODULE_OK;
	}

	const uint64_t ndicts = RedisModule_LoadUnsigned(rdb);
	RedisModule_Log(NULL, "debug", "Loading %llu dicts", ndicts);

	for (uint64_t i = 0; i < ndicts; i++) {
		const uint64_t id = RedisModule_LoadUnsigned(rdb);
		const uint64_t prefix_len = RedisModule_LoadUnsigned(rdb);
		char *prefix = NULL;

		if (prefix_len > 0) {
			prefix = RedisModule_LoadStringBuffer(rdb, NULL);
		}
		size_t buflen;
		char *const buf = RedisModule_LoadStringBuffer(rdb,
		    &buflen);

		RedisModule_Log(NULL, "debug", "Loading dict with ID %llu", id);
		long long ret = dict_create_with_id(&module, id, buf,
		    buflen, prefix, prefix_len, module.clevel);
		RedisModule_Free(buf);
		RedisModule_Free(prefix);
		if (ret < 0) {
			RedisModule_Log(NULL, "error",
			    "Failed to load dict %llu", id);
			return REDISMODULE_ERR;
		}
	}
	return REDISMODULE_OK;
}

const char *zipstr_decompress(struct compress_module *module,
    const struct zipstr *zs, size_t *buflen) {

	size_t orig_len;
	if (zs->dict != NULL) {
		orig_len = ZSTD_decompress_usingDDict(module->dctx,
		    module->buf, module->buflen, zs->buf, zs->len,
		    zs->dict->ddict);
	} else {
		orig_len = ZSTD_decompress(module->buf, module->buflen,
		    zs->buf, zs->len);
	}
	if (ZSTD_isError(orig_len) != 0) {
		return NULL;
	}

	*buflen = orig_len;
	return module->buf;
}

int SetCommand(RedisModuleCtx *ctx, RedisModuleString **argv,
    int argc) {

	if (argc != 3)
		return RedisModule_WrongArity(ctx);

	RedisModuleString *const keyname = argv[1];
	RedisModuleString *const val = argv[2];
	size_t src_len;
	const char *const src = RedisModule_StringPtrLen(val, &src_len);
	size_t key_len;
	const char *const keystr = RedisModule_StringPtrLen(keyname, &key_len);

	struct zipstr *const zs = zipstr_create(&module, keystr, key_len,
	    src, src_len);
	if (zs == NULL) {
		/* XXX Only do in transparent compression mode? */
		RedisModuleCallReply *reply;
		reply = RedisModule_Call(ctx, "SET", "v", &argv[1],
		    argc - 1);
		return RedisModule_ReplyWithCallReply(ctx, reply);
	}

	/* Data was compressed successfully; create an object */ 
	RedisModuleKey *const key = RedisModule_OpenKey(ctx, keyname,
	    REDISMODULE_WRITE);
	RedisModule_ModuleTypeSetValue(key, ZipString_Type, zs);
	RedisModule_CloseKey(key);

	return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int GetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
	if (argc != 2)
		return RedisModule_WrongArity(ctx);

	RedisModuleString *const keyname = argv[1];
	RedisModuleKey *const key = RedisModule_OpenKey(ctx, keyname,
	    REDISMODULE_READ);
	
	switch (RedisModule_KeyType(key)) {
	case REDISMODULE_KEYTYPE_MODULE:
	        if (RedisModule_ModuleTypeGetType(key) != ZipString_Type) {
			RedisModule_CloseKey(key);
			return RedisModule_ReplyWithError(ctx, "ERR bad type");
		}
	        break;
	case REDISMODULE_KEYTYPE_EMPTY:
		RedisModule_CloseKey(key);
		return RedisModule_ReplyWithNull(ctx);
	case REDISMODULE_KEYTYPE_STRING:
		/* XXX for now make a GET call. Should just get the string? */
		{
			RedisModuleCallReply *reply;
			reply = RedisModule_Call(ctx, "GET", "s", keyname);
			return RedisModule_ReplyWithCallReply(ctx, reply);
		}
	default:
		RedisModule_CloseKey(key);
		return RedisModule_ReplyWithError(ctx, "ERR bad type");
	}

	const struct zipstr *const zs = RedisModule_ModuleTypeGetValue(key);
	size_t buflen;
	const char *buf = zipstr_decompress(&module, zs, &buflen);

	RedisModule_CloseKey(key);

	if (buf == NULL) {
		RedisModule_ReplyWithError(ctx, "ERR decompression failed");
	}

	return RedisModule_ReplyWithStringBuffer(ctx, buf, buflen);
}

void command_filter(RedisModuleCommandFilterCtx *fctx, const char *replace_cmd,
    RedisModuleString *replace) {
	const RedisModuleString *cmd = RedisModule_CommandFilterArgGet(fctx, 0);
	const char *s = RedisModule_StringPtrLen(cmd, NULL);

	if (strcasecmp(replace_cmd, s) != 0) {
		return;
	}
	/*
	 * Increase ref count as Redis drops the refcnt of the arguments
	 * once the call has finished processing.
	 */
	RedisModule_RetainString(NULL, replace);
	RedisModule_CommandFilterArgReplace(fctx, 0, replace);
}

void set_command_filter(RedisModuleCommandFilterCtx *fctx) {
	command_filter(fctx, "set", module.zstd_set_str);
}

void get_command_filter(RedisModuleCommandFilterCtx *fctx) {
	command_filter(fctx, "get", module.zstd_get_str);
}


int TransparentCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
	if (argc != 2) {
		return RedisModule_WrongArity(ctx);
	}
	const char *val = RedisModule_StringPtrLen(argv[1], NULL);

	if (strcasecmp(val, "yes") == 0) {
		int c = 0;
		if (module.set_filter == NULL) {
			c++;
			module.set_filter = RedisModule_RegisterCommandFilter(
			    ctx, set_command_filter, 0);
		}
		if (module.get_filter == NULL) {
			c++;
			module.get_filter = RedisModule_RegisterCommandFilter(
			    ctx, get_command_filter, 0);
		}

		return RedisModule_ReplyWithLongLong(ctx, c);
	}

	if (strcasecmp(val, "no") == 0) {
		int c = 0;
		if (module.set_filter != NULL) {
			c++;
			RedisModule_UnregisterCommandFilter(ctx,
			    module.set_filter);
			module.set_filter = NULL;
		}
		if (module.get_filter != NULL) {
			c++;
			RedisModule_UnregisterCommandFilter(ctx,
			    module.get_filter);
			module.get_filter = NULL;
		}
		return RedisModule_ReplyWithLongLong(ctx, c);
	}

	return RedisModule_ReplyWithError(ctx, "invalid argument");
}

void train_callback(RedisModuleCtx *ctx, RedisModuleString *keyname,
    RedisModuleKey *key, void *data) {
	REDISMODULE_NOT_USED(ctx);

	struct train_data *const train = data;

	if (key == NULL)
		return;

	/* Ensure that the key matches the given prefix  */
	size_t keylen;
	const char *keystr = RedisModule_StringPtrLen(keyname, &keylen);

	if (train->match_prefix != NULL && (keylen < train->match_len ||
	    memcmp(train->match_prefix, keystr, train->match_len) != 0)) {
		RedisModule_Log(ctx, "notice", "prefix mismatch %s",
		    RedisModule_StringPtrLen(keyname, NULL));
		return;
	} else {
		RedisModule_Log(ctx, "notice", "prefix MATCH %s",
		    RedisModule_StringPtrLen(keyname, NULL));
	}

	/* Ensure there is space for another sample */
	if (train->nsamples >= train->max_nsamples ||
	    train->offset >= train->buflen) {
		return;
	}

	/* Only STRING objects can serve as sample data */
	if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_STRING)
		return;

	size_t sample_len;
	const char *const sample = RedisModule_StringDMA(key, &sample_len,
	    REDISMODULE_READ);
	if (sample == NULL)
		return;

	/* Partial data is OK */
	if (sample_len + train->offset >= train->buflen)
		sample_len = train->buflen - train->offset;

	memcpy(train->buf + train->offset, sample, sample_len);

	train->offset += sample_len;
	train->sample_sizes[train->nsamples] = sample_len;
	train->nsamples++;
}

#define	DEFAULT_DICT_SIZE	100*1024
#define	DEFAULT_MAX_NSAMPLES	1024
#define	TRAINBUF_FACTOR		10

/*
 * DICT TRAIN [DICTSIZE <size>] [PREFIX <prefix>]
 *
 * Train a new dictionary on STRING objects stored in Redis.
 *
 * Options
 *
 * The following options are supported.
 *
 * DICTSIZE bytes -- Target size of the dictionary (default 100 KB).
 *
 * PREFIX string  -- Train data only on strings where the keys match the
 *                   prefix. 
 *
 */
int DictTrainCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
	if (argc != 2 && argc != 4) {
		return RedisModule_WrongArity(ctx);
	}

	long long dict_size = DEFAULT_DICT_SIZE;
	const char *prefix = NULL;
	size_t prefix_len = 0;

	if (argc > 2 && (argc % 2) != 0) {
		/* expect matching OPTION VAL pairs */
		return RedisModule_ReplyWithError(ctx,
		    "ERR invalid syntax");
	}
	for (int i = 2; i < argc; i += 2) {
		const char *const arg = RedisModule_StringPtrLen(argv[i], NULL);
		RedisModuleString *const val = argv[i+1];

		if (strcasecmp(arg, "dictsize") == 0) {
			int err = RedisModule_StringToLongLong(val, &dict_size);
			if (err != REDISMODULE_OK) {
				return RedisModule_ReplyWithError(ctx,
				    "ERR invalid dictsize");
			}
			if (dict_size < ZDICT_DICTSIZE_MIN) {
				return RedisModule_ReplyWithError(ctx,
				    "ERR dictsize is too small");
			}
		} else if (strcasecmp(arg, "prefix") == 0) {
			prefix = RedisModule_StringPtrLen(val, &prefix_len);
		} else {
			return RedisModule_ReplyWithError(ctx,
			    "ERR invalid syntax");
		}
	}

	struct train_data train;
	train.buflen = TRAINBUF_FACTOR * dict_size;
	train.buf = RedisModule_Alloc(train.buflen);
	train.offset = 0;
	train.max_nsamples = DEFAULT_MAX_NSAMPLES;
	train.sample_sizes = RedisModule_Calloc(train.max_nsamples,
	    sizeof (*train.sample_sizes));
	train.nsamples = 0;
	train.match_prefix = prefix;
	train.match_len = prefix_len;

	RedisModuleScanCursor *const c = RedisModule_ScanCursorCreate();
	int iter = 0;
	int active = 0;
	RedisModule_Log(ctx, "debug", "Start scan for training data");
	do {
		RedisModule_Log(ctx, "debug", "iteration %d", iter++);
		active = RedisModule_Scan(ctx, c, train_callback, &train);
	} while (active == 1 && train.nsamples < train.max_nsamples &&
	    train.offset < train.buflen); 
	RedisModule_ScanCursorDestroy(c);
	RedisModule_Log(ctx, "debug", "End scan. %zu samples, buf size %zu",
	    train.nsamples, train.offset);

	/*
	 * Attempt to create a dictionary from training data.
	 */
	char *dictbuf = RedisModule_Alloc(dict_size);
	dict_size = ZDICT_trainFromBuffer(dictbuf, dict_size,
		train.buf, train.sample_sizes, train.nsamples);

	RedisModule_Free(train.buf);
	RedisModule_Free(train.sample_sizes);

	if (ZSTD_isError(dict_size)) {
		const int err = ZSTD_getErrorCode(dict_size);
		const char *errstr = ZSTD_getErrorString(err);
		const char *fmt = "ERR zstd error: %s";
		char buf[strlen(errstr) + strlen(fmt)];

		(void) snprintf(buf, sizeof (buf), fmt, errstr);

		return RedisModule_ReplyWithError(ctx, buf);
	}

	long long id = dict_create(&module, dictbuf, dict_size, prefix,
	    prefix_len, module.clevel);
	RedisModule_Free(dictbuf);

	if (id < 0) {
		return RedisModule_ReplyWithError(ctx, "ERR dictionary failed");
	}

	RedisModule_ReplyWithArray(ctx, 3);
	RedisModule_ReplyWithLongLong(ctx, id);
	RedisModule_ReplyWithLongLong(ctx, dict_size);
	RedisModule_ReplyWithLongLong(ctx, train.nsamples);

	return REDISMODULE_OK;
}

int DictDropCommand(RedisModuleCtx *ctx, struct dict *dict) {
	if (dict == NULL) {
		return RedisModule_ReplyWithError(ctx,
		    "ERR no active dictionary");
	}

	dict_rele(&module, dict, NULL);
	if (module.dict == dict)
		module.dict = NULL;

	return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

int DictDumpCommand(RedisModuleCtx *ctx, const struct dict *dict) {
	if (dict == NULL) {
		return RedisModule_ReplyWithNull(ctx);
	}
	return RedisModule_ReplyWithStringBuffer(ctx, dict->buf,
	    dict->buflen);
}

int DictListCommand(RedisModuleCtx *ctx) {
	RedisModule_ReplyWithArray(ctx, RedisModule_DictSize(module.all_dicts));

	RedisModuleDictIter *const iter = RedisModule_DictIteratorStartC(
	    module.all_dicts, "^", NULL, 0);

	void *data;
	while (RedisModule_DictNextC(iter, NULL, &data) != NULL) {
		const struct dict *dict = data;
		double ratio = 0;
		const char *prefix = "";
		size_t prefix_len = 1;

		if (dict->prefix_len > 0) {
			prefix = dict->prefix;
			prefix_len = dict->prefix_len;
		}
		if (dict->mem_compressed > 0) {
			ratio = (double)dict->mem_uncompressed /
				(double)dict->mem_compressed;
		}

		RedisModule_ReplyWithArray(ctx, 6);
		RedisModule_ReplyWithLongLong(ctx, dict->id);
		RedisModule_ReplyWithStringBuffer(ctx, prefix, prefix_len);
		RedisModule_ReplyWithLongLong(ctx, dict->refcnt);
		RedisModule_ReplyWithLongLong(ctx, dict->mem_uncompressed);
		RedisModule_ReplyWithLongLong(ctx, dict->mem_compressed);
		RedisModule_ReplyWithDouble(ctx, ratio);
	}
	RedisModule_DictIteratorStop(iter);

	return REDISMODULE_OK;
}

/*
 *  Perform actions on zstd dictionaries.
 */
int DictCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
	const char *const help[] = {
		"DICT subcommands are:",
		"LIST                    -- List active dictionaries.",
		"DUMP [<id>]             -- Dump the dictionary.",
		"RESTORE <DICTBUF>       -- Restores the dictionary",
		"DROP                    -- Drops the dictionary.",
		"TRAIN [DICTSIZE <size>] -- Train a new dictionary.",
	};

	if (argc < 2) {
		return RedisModule_WrongArity(ctx);
	}

	RedisModuleString *const subcmd = argv[1];
	const char *str = RedisModule_StringPtrLen(subcmd, NULL);

	if (strcasecmp(str, "train") == 0) {
		/* DICT TRAIN */
		return DictTrainCommand(ctx, argv, argc);
	} else if (strcasecmp(str, "restore") == 0) {
		/* DICT RESTORE <dictBuffer> */
		if (argc != 3) {
			return RedisModule_WrongArity(ctx);
		}
		size_t buflen;
		const char *buf = RedisModule_StringPtrLen(argv[2], &buflen);

		if (dict_create(&module, buf, buflen, NULL, 0,
		    module.clevel) < 0) {
			return RedisModule_ReplyWithError(ctx,
			    "ERR dictionary failed");
		}
		return RedisModule_ReplyWithSimpleString(ctx, "OK");
	} else if (
	    strcasecmp(str, "drop") == 0 ||
	    strcasecmp(str, "dump") == 0) {
		/* DICT DROP [<id>] */
		/* DICT DUMP [<id>] */

		struct dict *dict;
		long long id;

		switch (argc) {
		case 2:
			dict = module.dict;
			break;
		case 3: /* ID specified */

			if (RedisModule_StringToLongLong(argv[2], &id) ==
			    REDISMODULE_ERR) {
				return RedisModule_ReplyWithError(ctx,
				    "ERR invalid dictionary id");
			}
			dict = RedisModule_DictGetC(module.all_dicts,
			    (void *)&id, sizeof (id), NULL);
			break;
		default:
			return RedisModule_WrongArity(ctx);
		}

		if (strcasecmp(str, "drop") == 0)
			return DictDropCommand(ctx, dict);
		return DictDumpCommand(ctx, dict);
	} else if (strcasecmp(str, "list") == 0) {
		/* DICT LIST */
		DictListCommand(ctx);
	} else if (strcasecmp(str, "help") == 0) {
		/* DICT HELP */
		size_t items = sizeof (help) / sizeof (help[0]);
		RedisModule_ReplyWithArray(ctx, items);
		for (size_t i = 0; i < items; i++) {
			RedisModule_ReplyWithSimpleString(ctx, help[i]);
		}
		return REDISMODULE_OK;
	}

	return RedisModule_ReplyWithError(ctx,
	    "Unknown subcommand. Try DICT HELP.");
}

RedisModuleString *zstd_set_str;

void info_cb(RedisModuleInfoCtx *ictx, int for_crash_report) {
	REDISMODULE_NOT_USED(for_crash_report);

	double ratio = (double)module.mem_total_uncompressed /
		(double)module.mem_total_compressed;
	char buf[10];

	RedisModule_InfoAddSection(ictx, "stats");
	RedisModule_InfoAddFieldULongLong(ictx, "zstd_version",
	    ZSTD_versionNumber());
	snprintf(buf, sizeof(buf), "%#.2f", ratio);
	RedisModule_InfoAddFieldCString(ictx, "compression_ratio", buf);
	RedisModule_InfoAddFieldULongLong(ictx, "compressed_size",
	    module.mem_total_compressed);
	RedisModule_InfoAddFieldULongLong(ictx, "uncompressed_size",
	    module.mem_total_uncompressed);
	RedisModule_InfoAddFieldULongLong(ictx, "objects",
	    module.nobjs);
	RedisModule_InfoAddFieldULongLong(ictx, "dictionaries",
	    RedisModule_DictSize(module.all_dicts));
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
	REDISMODULE_NOT_USED(argv);
	REDISMODULE_NOT_USED(argc);

	if (RedisModule_Init(ctx, MODPREFIX, 1, REDISMODULE_APIVER_1) ==
	    REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	memset(&module, 0, sizeof (module));
	module.buflen = BUFSIZE;
	module.buf = RedisModule_Alloc(module.buflen);
	module.clevel = ZSTD_CLEVEL_DEFAULT; 
	module.dict = NULL;
	module.cctx = ZSTD_createCCtx();
	module.dctx = ZSTD_createDCtx();
	module.all_dicts = RedisModule_CreateDict(ctx);
	module.prefix_dicts = RedisModule_CreateDict(ctx);
	module.set_filter = NULL;
	module.zstd_set_str = RedisModule_CreateString(ctx, MODPREFIX".set", 8); 
	module.get_filter = NULL;
	module.zstd_get_str = RedisModule_CreateString(ctx, MODPREFIX".get", 8); 

	RedisModule_Log(NULL, "debug", "Registering type ver %d",
	    REDISMODULE_TYPE_METHOD_VERSION);

	RedisModuleTypeMethods tm = {
		.version = REDISMODULE_TYPE_METHOD_VERSION,
		.rdb_save = zipstr_rdb_save,
		.rdb_load = zipstr_rdb_load,
		.aux_save_triggers = REDISMODULE_AUX_BEFORE_RDB,
		.aux_save = zipstr_aux_save,
		.aux_load = zipstr_aux_load,
		.free = zipstr_free
	};

	ZipString_Type = RedisModule_CreateDataType(ctx, "ZipStr001",
	    ZIPSTR_ENCODING_VERSION, &tm);
	if (ZipString_Type == NULL)
		return REDISMODULE_ERR;

	RedisModule_RegisterInfoFunc(ctx, info_cb);

	if (RedisModule_CreateCommand(ctx, MODPREFIX".set", SetCommand,
	    "write", 1, 1, 1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_CreateCommand(ctx, MODPREFIX".get", GetCommand,
	    "readonly", 1, 1, 1) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_CreateCommand(ctx, MODPREFIX".dict", DictCommand,
	    "admin", 0, 0, 0) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	if (RedisModule_CreateCommand(ctx, MODPREFIX".transparent",
	    TransparentCommand, "admin", 0, 0, 0) == REDISMODULE_ERR) {
		return REDISMODULE_ERR;
	}

	return REDISMODULE_OK;
}

int RedisModule_OnUnload(RedisModuleCtx *ctx) {
	REDISMODULE_NOT_USED(ctx);
	/*
	 * Redis does not currently allow modules with data types to unloaded,
	 * so this is not strictly needed. 
	 */
	return REDISMODULE_ERR;
}
