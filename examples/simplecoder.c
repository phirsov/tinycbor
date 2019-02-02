#include "../src/cbor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>


struct Token
{
    union
    {
        long int vali;
        double vald;
        char *vals;
    };
    enum {None, Array, Map, Int, Double, String, OpeningBracket, ClosingBracket,
        Null, Bool, Undefined} type;
    size_t lineno;
    struct Token *next;
};

typedef struct Token Token;


struct EncoderContext
{
    Token *tokens, *curtoken;
    size_t nestinglvl;
    CborEncoder encoder;
    uint8_t *outbuff;
    size_t outbuffsz;
};

typedef struct EncoderContext EncoderContext;


static void complain(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void complainline(const char *msg, size_t lineno)
{
    fprintf(stderr, "%s at line: %lu\n", msg, (unsigned long)lineno);
}

static void complainstr(const char *msg, const char *s)
{
    fprintf(stderr, "%s: %s\n", msg, s);
}

static void complainerrno(const char *msg, int err)
{
    complainstr(msg, strerror(err));
}

static void complainencode(CborError err, size_t lineno)
{
    fprintf(stderr, "encoder error: %s at line %lu\n",
        cbor_error_string(err), (unsigned long)lineno);
}


static Token *alloctoken(int type, void *data, size_t size)
{
    Token *result = malloc(sizeof(Token));

    if (!result) {
        complain("token allocation failure");
        goto alloctokenfail;
    }
    switch (type) {
        case Int:
            result->vali = *(long int*)data;
            break;

        case Double:
            result->vald = *(double*)data;
            break;

        case Bool:
            result->vali = *(long int*)data;
            break;

        case String:
            ++size;
            if (!size) {
                complain("string token buffer out of bounds");
            } else if (!(result->vals = malloc(size))) {
                complain("string token buffer allocation failure");
            } else {
                memcpy(result->vals, data, size - 1);
                result->vals[size - 1] = '\0';
                break;
            }
            goto alloctokenfail;

        default:
            break;
    }

    result->type = type;
    return result;

alloctokenfail:
    free(result);
    return NULL;
}

static void freetoken(Token *token)
{
    if (token->type == String) {
        free(token->vals);
    }
    free(token);
}


static const char *trailingspace(const char *str)
{
    const char *result = str;
    for ( ; *str; ++str) {
        if (!isspace(*str)) {
            result = str + 1;
        }
    }
    return result;
}


static size_t skipws(const char *str)
{
    const char *begin = str;
    for ( ; *str && isspace(*str); ++str) {
    }
    return str - begin;
}

static size_t skipalnumtoken(const char *str, const char *tokenstr)
{
    size_t len = strlen(tokenstr);
    if (len && !strncmp(str, tokenstr, len) && !(str[len] && isalnum(str[len]))) {
        return len;
    };
    return 0;
}

static size_t skipchartoken(const char *str, char tokenc)
{
    return str[0] == tokenc ? 1 : 0;
}

static size_t skipinttoken(const char *str, long int *out)
{
    char *end;
    *out = strtol(str, &end, 0);
    return end - str;
}

static size_t skipdoubletoken(const char *str, double *out)
{
    char *end;
    *out = strtod(str, &end);
    return end - str;
}

static size_t skipstrtoken(const char *str, const char **pbegin,
        const char **pend)
{
    if (*str == '"') {
        const char *end = str;
        for (++end; *end && *end != '"'; ++end) {
        }
        if (*end == '"') {
            *pbegin = str + 1;
            *pend = end;
            return end - str + 1;
        }
    }
    return 0;
}

static size_t skipalnumtokens(const char *str, int *outtype)
{
    struct
    {
        const char *tokenstr;
        int type;
    } patterns[] = {
        {"Array", Array},
        {"Map", Map},
        {"null", Null},
        {"undefined", Undefined},
        {NULL, 0}
    };

    size_t i;

    for (i = 0; patterns[i].tokenstr; ++i) {
        size_t result;
        if ((result = skipalnumtoken(str, patterns[i].tokenstr))) {
            *outtype = patterns[i].type;
            return result;
        }
    }

    return 0;
}

static size_t skipchartokens(const char *str, int *outtype)
{
    struct
    {
        char tokenc;
        int type;
    } patterns[] = {
        {'[', OpeningBracket},
        {']', ClosingBracket},
        {'\0', 0}
    };

    size_t i;

    for (i = 0; patterns[i].tokenc; ++i) {
        if (skipchartoken(str, patterns[i].tokenc)) {
            *outtype = patterns[i].type;
            return 1;
        }
    }

    return 0;
}

static Token *firsttoken(const char *str, const char **pend)
{
    const char *begin, *end;
    int type;
    void *data = NULL;
    size_t size;
    size_t skip;
    long int vali;
    double vald;

    str += skipws(str);

    if (!*str) {
        type = None;
        skip = 0;
    } else if ((skip = skipalnumtokens(str, &type))) {
    } else if ((skip = skipchartokens(str, &type))) {
    } else if ((skip = skipstrtoken(str, &begin, &end))) {
        type = String;
        data = (void*)begin;
        size = end - begin;
    } else if ((skip = skipalnumtoken(str, "true"))) {
        type = Bool;
        vali = 1;
        data = (void*)&vali;
    } else if ((skip = skipalnumtoken(str, "false"))) {
        type = Bool;
        vali = 0;
        data = (void*)&vali;
    } else {
        size_t skipd = skipdoubletoken(str, &vald),
               skipi = skipinttoken(str, &vali);
        if (skipd > skipi) {
            skip = skipd;
            type = Double;
            data = (void*)&vald;
        } else if (skipi) {
            skip = skipi;
            type = Int;
            data = (void*)&vali;
        }
    }

    if (*str && !skip) {
        complainstr("token not recognized", str);
        return NULL;
    }

    *pend = str + skip;
    return alloctoken(type, data, size);
}

static int readtokens(EncoderContext *ctx, FILE *file)
{
    char linebuff[1024];
    size_t lineno = 0;

    while (fgets(linebuff, sizeof(linebuff), file)) {
        *(char*)trailingspace(linebuff) = '\0';
        const char *ptr = linebuff, *endptr;
        ++lineno;

        for ( ; *ptr; ptr = endptr) {
            Token *token = firsttoken(ptr, &endptr);
            if (!token) {
                complain("read tokens failure");
                return 1;
            } else if (token->type == None) {
                freetoken(token);
            } else {
                token->lineno = lineno;
                if (!ctx->curtoken) {
                    ctx->tokens = token;
                    ctx->curtoken = token;
                } else {
                    ctx->curtoken->next = token;
                    ctx->curtoken = token;
                }
            }
        }
    }
    return 0;
}


typedef CborError (*encoder_create_container_fn)
        (CborEncoder * , CborEncoder * , size_t);

static int encoderecursive(EncoderContext *ctx);

static int encodecontainerhelper(EncoderContext *ctx,
        encoder_create_container_fn create_container)
{
    Token *token = ctx->curtoken;
    CborError err;
    EncoderContext nestedctx;

    memcpy(&nestedctx, ctx, sizeof(*ctx));
    nestedctx.curtoken = token->next;
    ++nestedctx.nestinglvl;

    if (!nestedctx.curtoken) {
        complainline("unexpected EOF", token->lineno);
    } else if (nestedctx.curtoken->type != OpeningBracket) {
        complainline("missing opening bracket", nestedctx.curtoken->lineno);
    } else if ((err = create_container(&ctx->encoder, &nestedctx.encoder,
            CborIndefiniteLength) != CborNoError)) {
        complainencode(err, token->lineno);
    } else if (!(nestedctx.curtoken = nestedctx.curtoken->next) ||
            encoderecursive(&nestedctx)) {
        complainline("encode container failure", token->lineno);
    } else if (nestedctx.nestinglvl != ctx->nestinglvl) {
        complainline("unbalanced nesting level", token->lineno);
    } else if ((err = cbor_encoder_close_container(&ctx->encoder,
            &nestedctx.encoder) != CborNoError)) {
        complainencode(err, token->lineno);
    } else {
        ctx->curtoken = nestedctx.curtoken;
        return 0;
    }
    return 1;
}

static int encodearrayhelper(EncoderContext *ctx)
{
    return encodecontainerhelper(ctx, cbor_encoder_create_array);
}

static int encodemaphelper(EncoderContext *ctx)
{
    return encodecontainerhelper(ctx, cbor_encoder_create_map);
}

static int encoderecursive(EncoderContext *ctx)
{
    for ( ; ctx->curtoken; ctx->curtoken = ctx->curtoken->next) {
        Token *token = ctx->curtoken;
        CborError err = CborNoError;
        switch (token->type) {
            case Array:
                if (encodearrayhelper(ctx)) {
                    return 1;
                }
                break;

            case Map:
                if (encodemaphelper(ctx)) {
                    return 1;
                }
                break;

            case ClosingBracket:
                if (ctx->nestinglvl > 0) {
                    --ctx->nestinglvl;
                    return 0;
                } else {
                    complainline("unexpected bracket", token->lineno);
                    return 1;
                }

            case Int:
                err = cbor_encode_int(&ctx->encoder, token->vali);
                break;

            case Double:
                err = cbor_encode_double(&ctx->encoder, token->vald);
                break;

            case String:
                err = cbor_encode_text_stringz(&ctx->encoder, token->vals);
                break;

            case Null:
                err = cbor_encode_null(&ctx->encoder);
                break;

            case Bool:
                err = cbor_encode_boolean(&ctx->encoder, token->vali != 0);
                break;

            case Undefined:
                err = cbor_encode_undefined(&ctx->encoder);
                break;

            default:
                complainline("unhandled token", token->lineno);
                return 1;
        }
        if (err != CborNoError) {
            complainencode(err, token->lineno);
            return 1;
        }
    }
    return 0;
}

static int initcontext(EncoderContext *ctx, size_t outbuffsz)
{
    memset(ctx, 0, sizeof(*ctx));
    if (!(ctx->outbuff = malloc(outbuffsz))) {
        complain("coder ctx out buffer allocation failure");
    } else {
        ctx->outbuffsz = outbuffsz;
        cbor_encoder_init(&ctx->encoder, ctx->outbuff, ctx->outbuffsz, 0);
        return 0;
    }
    return 1;
}

static int readfile(EncoderContext *ctx, const char *filename)
{
    FILE *stream = fopen(filename, "r");
    if (!stream) {
        complainerrno("file open failure", errno);
    } else {
        int err = readtokens(ctx, stream);
        if (fclose(stream)) {
            complainerrno("file close failure", errno);
            err = 1;
        }
        return err;
    }
    return 1;
}

static int encode(EncoderContext *ctx)
{
    ctx->curtoken = ctx->tokens;
    return encoderecursive(ctx);
}

static int dump(EncoderContext *ctx)
{
    uint8_t *p = ctx->outbuff;
    size_t sz = cbor_encoder_get_buffer_size(&ctx->encoder, p);
    uint8_t *end = p + sz;
    while (p < end) {
        fprintf(stdout, "%02x ", *p++);
    }
    fprintf(stdout, "\n");
    return 0;
}

static void cleanupcontext(EncoderContext *ctx)
{
    Token *token;
    while ((token = ctx->tokens)) {
        ctx->tokens = token->next;
        freetoken(token);
    }
    free(ctx->outbuff);
}


int main(int argc, char **argv)
{
    enum {AppName, FileName, BuffSize, ArgCount};

    if (argc != ArgCount) {
        puts("simplecoder <filename> <bufsize>");
        return 0;
    }

    size_t buff_size = atol(argv[BuffSize]);
    int result = 1;
    EncoderContext ctx;

    if (initcontext(&ctx, buff_size)) {
        complain("context initialization failure");
    } else if (readfile(&ctx, argv[FileName])) {
        complain("read file failure");
    } else if (encode(&ctx)) {
        complain("encode failure");
    } else if (dump(&ctx)) {
        complain("dump failure");
    } else {
        result = 0;
    }
    cleanupcontext(&ctx);
    return result;
}
