/*
  +----------------------------------------------------------------------+
  | XHP                                                                  |
  +----------------------------------------------------------------------+
  | Copyright (c) 1998-2014 Zend Technologies Ltd. (http://www.zend.com) |
  | Copyright (c) 2009-2014 Facebook, Inc. (http://www.facebook.com)     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.00 of the Zend license,     |
  | that is bundled with this package in the file LICENSE.ZEND, and is   |
  | available through the world-wide-web at the following url:           |
  | http://www.zend.com/license/2_00.txt.                                |
  | If you did not receive a copy of the Zend license and are unable to  |
  | obtain it through the world-wide-web, please send a note to          |
  | license@zend.com so we can mail you a copy immediately.              |
  +----------------------------------------------------------------------+
*/

#include "ext.hpp"
#include "xhp/xhp_preprocess.hpp"
#include "php.h"
#include "php_ini.h"
#include "zend.h"
#include "zend_API.h"
#include "zend_compile.h"
#include "zend_operators.h"
#include "zend_hash.h"
#include "zend_extensions.h"
#include "ext/standard/info.h"
#include <string>
#include <sstream>
#include <iostream>

#ifdef ZEND_ENGINE_3
# define XHP_ZVAL_STRINGL(pzv, str, len) ZVAL_STRINGL(pzv, str, len)
# define xhp_add_assoc_string(pzv, key, val) add_assoc_string(pzv, key, val)
# define CG5(v) 0
#else
# define XHP_ZVAL_STRINGL(pzv, str, len) ZVAL_STRINGL(pzv, str, len, 1)
# define xhp_add_assoc_string(pzv, key, val) add_assoc_string(pzv, key, val, 1)
# define CG5(v) CG(v)
typedef long zend_long;
#endif

using namespace std;

//
// Decls
typedef zend_op_array* (zend_compile_file_t)(zend_file_handle*, int TSRMLS_DC);
typedef zend_op_array* (zend_compile_string_t)(zval*, char* TSRMLS_DC);
static zend_compile_file_t* dist_compile_file;
static zend_compile_string_t* dist_compile_string;

typedef struct {
  const char* str;
  size_t pos;
  size_t len;
} xhp_stream_t;

//
// Globals
ZEND_BEGIN_MODULE_GLOBALS(xhp)
  bool idx_expr;
  bool include_debug;
  bool force_global_namespace;
ZEND_END_MODULE_GLOBALS(xhp)
ZEND_DECLARE_MODULE_GLOBALS(xhp)

#ifdef ZTS
# define XHPG(i) TSRMG(xhp_globals_id, zend_xhp_globals*, i)
#else
# define XHPG(i) (xhp_globals.i)
#endif

//
// PHP 5.3 helper functions
#if PHP_VERSION_ID >= 50300

// These functions wese made static to zend_stream.c in r255174. These an inline copies of those functions.
static int zend_stream_getc(zend_file_handle *file_handle TSRMLS_DC) {
  char buf;

  if (file_handle->handle.stream.reader(file_handle->handle.stream.handle, &buf, sizeof(buf) TSRMLS_CC)) {
    return (int)buf;
  }
  return EOF;
}

static size_t zend_stream_read(zend_file_handle *file_handle, char *buf, size_t len TSRMLS_DC) {
  if (file_handle->type != ZEND_HANDLE_MAPPED && file_handle->handle.stream.isatty) {
    int c = '*';
    size_t n;

#ifdef NETWARE
    /*
      c != 4 check is there as fread of a character in NetWare LibC gives 4 upon ^D character.
      Ascii value 4 is actually EOT character which is not defined anywhere in the LibC
      or else we can use instead of hardcoded 4.
    */
    for (n = 0; n < len && (c = zend_stream_getc(file_handle TSRMLS_CC)) != EOF && c != 4 && c != '\n'; ++n) {
#else
    for (n = 0; n < len && (c = zend_stream_getc(file_handle TSRMLS_CC)) != EOF && c != '\n'; ++n)  {
#endif
        buf[n] = (char)c;
    }
    if (c == '\n') {
      buf[n++] = (char)c;
    }

    return n;
  }
  return file_handle->handle.stream.reader(file_handle->handle.stream.handle, buf, len TSRMLS_CC);
}
#endif

//
// XHP Streams
size_t xhp_stream_reader(xhp_stream_t* handle, char* buf, size_t len TSRMLS_DC) {
  if (len > handle->len - handle->pos) {
    len = handle->len - handle->pos;
  }
  if (len) {
    memcpy(buf, handle->str + handle->pos, len);
    buf[len] = 0;
    handle->pos += len;
    return len;
  } else {
    return 0;
  }
}

long xhp_stream_fteller(xhp_stream_t* handle TSRMLS_DC) {
  return (long)handle->pos;
}

//
// PHP compilation intercepter
static zend_op_array* xhp_compile_file(zend_file_handle* f, int type TSRMLS_DC) {

  if (open_file_for_scanning(f TSRMLS_CC) == FAILURE) {
    // If opening the file fails just send it to the original func
    return dist_compile_file(f, type TSRMLS_CC);
  }

  // Grab code from zend file handle
  string original_code;
#if PHP_VERSION_ID >= 50300
  if (f->type == ZEND_HANDLE_MAPPED) {
    original_code = f->handle.stream.mmap.buf;
  }
#else
  if (0);
#endif
  else {

    // Read full program from zend stream
    char read_buf[4096];
    size_t len;
    while (len = zend_stream_read(f, (char*)&read_buf, 4095 TSRMLS_CC)) {
      read_buf[len] = 0;
      original_code += read_buf;
    }
  }

  // Process XHP
  XHPResult result;
  xhp_flags_t flags;
  string rewrit, error_str;
  uint32_t error_lineno;
  string* code_to_give_to_php;

  memset(&flags, 0, sizeof(xhp_flags_t));
  flags.asp_tags = CG5(asp_tags);
  flags.short_tags = CG5(short_tags);
  flags.idx_expr = XHPG(idx_expr);
  flags.include_debug = XHPG(include_debug);
#if PHP_VERSION_ID >= 50300
  flags.force_global_namespace = XHPG(force_global_namespace);
#else
  flags.force_global_namespace = false;
#endif
  result = xhp_preprocess(original_code, rewrit, error_str, error_lineno, flags);

  if (result == XHPErred) {
    // Bubble error up to PHP
    CG(in_compilation) = true;
    CG(zend_lineno) = error_lineno;
#ifdef ZEND_ENGINE_3
    {
      zend_string *str = zend_string_init(f->filename, strlen(f->filename), 0);
      zend_set_compiled_filename(str);
      zend_string_release(str);
    }
#else
    zend_set_compiled_filename(const_cast<char*>(f->filename) TSRMLS_CC);
#endif
    zend_error(E_PARSE, "%s", error_str.c_str());
    zend_bailout();
  } else if (result == XHPRewrote) {
    code_to_give_to_php = &rewrit;
  } else {
    code_to_give_to_php = &original_code;
  }

  // Create a fake file to give back to PHP to handle
  zend_file_handle fake_file;
#ifdef ZEND_ENGINE_3
  fake_file.opened_path = f->opened_path ? zend_string_copy(f->opened_path) : NULL;
#else
  fake_file.opened_path = f->opened_path ? estrdup(f->opened_path) : NULL;
#endif
  fake_file.filename = f->filename;
  fake_file.free_filename = false;

#if PHP_VERSION_ID >= 50300
  fake_file.handle.stream.isatty = 0;
  fake_file.handle.stream.mmap.pos = 0;
  fake_file.handle.stream.mmap.buf = const_cast<char*>(code_to_give_to_php->c_str());
  fake_file.handle.stream.mmap.len = code_to_give_to_php->size();
  fake_file.handle.stream.closer = NULL;
  fake_file.type = ZEND_HANDLE_MAPPED;
#else
  // This code works fine in PHP 5.3, but the mmap method is faster
  xhp_stream_t stream_handle;
  stream_handle.str = code_to_give_to_php->c_str();
  stream_handle.pos = 0;
  stream_handle.len = code_to_give_to_php->size();

  fake_file.type = ZEND_HANDLE_STREAM;
  fake_file.handle.stream.handle = &stream_handle;
  fake_file.handle.stream.reader = (zend_stream_reader_t)&xhp_stream_reader;
#if PHP_VERSION_ID >= 50300
  fake_file.handle.stream.fsizer = (zend_stream_fsizer_t)&xhp_stream_fteller;
#else
  fake_file.handle.stream.fteller = (zend_stream_fteller_t)&xhp_stream_fteller;
#endif
  fake_file.handle.stream.closer = NULL;
  fake_file.handle.stream.interactive = 0;
#endif

  zend_op_array* ret = dist_compile_file(&fake_file, type TSRMLS_CC);
  return ret;
}

static zend_op_array* xhp_compile_string(zval* str, char *filename TSRMLS_DC) {

  // Cast to str
  zval tmp;
  char* val;
  if (Z_TYPE_P(str) != IS_STRING) {
    tmp = *str;
    zval_copy_ctor(&tmp);
    convert_to_string(&tmp);
    val = Z_STRVAL(tmp);
  } else {
    val = Z_STRVAL_P(str);
  }

  // Process XHP
  string rewrit, error_str;
  string* code_to_give_to_php;
  uint32_t error_lineno;
  string original_code(val);
  xhp_flags_t flags;

  memset(&flags, 0, sizeof(xhp_flags_t));
  flags.asp_tags = CG5(asp_tags);
  flags.short_tags = CG5(short_tags);
  flags.idx_expr = XHPG(idx_expr);
  flags.include_debug = XHPG(include_debug);
  flags.force_global_namespace = XHPG(force_global_namespace);
  flags.eval = true;
  XHPResult result = xhp_preprocess(original_code, rewrit, error_str, error_lineno, flags);

  // Destroy temporary in the case of non-string input (why?)
  if (Z_TYPE_P(str) != IS_STRING) {
    zval_dtor(&tmp);
  }

  if (result == XHPErred) {

    // Bubble error up to PHP
    bool original_in_compilation = CG(in_compilation);
    CG(in_compilation) = true;
    CG(zend_lineno) = error_lineno;
    zend_error(E_PARSE, "%s", error_str.c_str());
    CG(unclean_shutdown) = 1;
    CG(in_compilation) = original_in_compilation;
    return NULL;
  } else if (result == XHPRewrote) {

    // Create another tmp zval with the rewritten PHP code and pass it to the original function
    XHP_ZVAL_STRINGL(&tmp, rewrit.c_str(), rewrit.size());
    zend_op_array* ret = dist_compile_string(&tmp, filename TSRMLS_CC);
    zval_dtor(&tmp);
    return ret;
  } else {
    return dist_compile_string(str, filename TSRMLS_CC);
  }
}

//
// globals initialization
static void php_xhp_init_globals(zend_xhp_globals* xhp_globals) {
  xhp_globals->idx_expr = false;
  xhp_globals->include_debug = true;
  xhp_globals->force_global_namespace = true;
}

//
// ini entry
PHP_INI_BEGIN()
  STD_PHP_INI_BOOLEAN("xhp.idx_expr", "0", PHP_INI_PERDIR, OnUpdateBool, idx_expr, zend_xhp_globals, xhp_globals)
  STD_PHP_INI_BOOLEAN("xhp.include_debug", "1", PHP_INI_PERDIR, OnUpdateBool, include_debug, zend_xhp_globals, xhp_globals)
  STD_PHP_INI_BOOLEAN("xhp.force_global_namespace", "1", PHP_INI_PERDIR, OnUpdateBool, force_global_namespace, zend_xhp_globals, xhp_globals)
PHP_INI_END()

//
// Extension entry
static PHP_MINIT_FUNCTION(xhp) {

  ZEND_INIT_MODULE_GLOBALS(xhp, php_xhp_init_globals, NULL);

  REGISTER_INI_ENTRIES();

#ifndef ZEND_ENGINE_3
  // APC has this crazy magic api you can use to avoid the race condition for when an extension overwrites
  // the compile_file function. The desired order here is APC -> XHP -> PHP, that way APC can cache the
  // file as usual.
  //
  // We can use module dependency as of 5.2, so just forget this for PHP7
  zend_module_entry *apc_lookup;
  zend_constant *apc_magic;
  if (zend_hash_find(&module_registry, "apc", sizeof("apc"), (void**)&apc_lookup) != FAILURE &&
      zend_hash_find(EG(zend_constants), "\000apc_magic", 11, (void**)&apc_magic) != FAILURE) {
    zend_compile_file_t* (*apc_set_compile_file)(zend_compile_file_t*) = (zend_compile_file_t* (*)(zend_compile_file_t*))apc_magic->value.value.lval;
    dist_compile_file = apc_set_compile_file(NULL);
    apc_set_compile_file(xhp_compile_file);
  } else
#endif
  {
    dist_compile_file = zend_compile_file;
    zend_compile_file = xhp_compile_file;
  }

  // For eval
  dist_compile_string = zend_compile_string;
  zend_compile_string = xhp_compile_string;
  return SUCCESS;
}

static PHP_MSHUTDOWN_FUNCTION(xhp) {
  UNREGISTER_INI_ENTRIES();
  return SUCCESS;
}

//
// phpinfo();
static PHP_MINFO_FUNCTION(xhp) {
  php_info_print_table_start();
  php_info_print_table_row(2, "Version", PHP_XHP_VERSION);
  php_info_print_table_row(2, "Include Debug Info Into XHP Classes", XHPG(include_debug) ? "enabled" : "disabled");
  php_info_print_table_row(2, "Manual Support For func_call()['key'] Syntax", XHPG(idx_expr) ? "enabled" : "disabled");
  php_info_print_table_row(2, "Force XHP Into The Global Namespace", XHPG(force_global_namespace) ? "enabled" : "disabled");
  php_info_print_table_end();
}

static inline void do_xhp_array(zval *return_value, zval *dict, zval *offset TSRMLS_DC) {
  switch (Z_TYPE_P(offset)) {
    case IS_NULL:
#ifdef ZEND_ENGINE_3
      if (zval *value = zend_hash_str_find(Z_ARRVAL_P(dict), "", 0)) {
        ZVAL_ZVAL(return_value, value, 1, 0);
        return;
      }
#else
      zval **value;
      if (zend_hash_find(Z_ARRVAL_P(dict), "", 0, (void**)&value) == SUCCESS) {
        ZVAL_ZVAL(return_value, *value, 1, 0);
        return;
      }
#endif
      zend_error(E_NOTICE, "Undefined index: ");
      RETURN_NULL();

#ifdef ZEND_ENGINE_3
    case IS_TRUE:
    case IS_FALSE: {
      zend_long lval = (Z_TYPE_P(offset) == IS_TRUE) ? 1 : 0;
      if (zval *value = zend_hash_index_find(Z_ARRVAL_P(dict), lval)) {
        ZVAL_ZVAL(return_value, value, 1, 0);
        return;
      }
      zend_error(E_NOTICE, "Undefined offset:  %ld", lval);
      RETURN_NULL();
    }
#endif

    case IS_RESOURCE:
      zend_error(E_STRICT, "Resource ID#%ld used as offset, casting to integer (%ld)", Z_LVAL_P(offset), Z_LVAL_P(offset));
      /* fallthrough */
    case IS_DOUBLE:
#ifndef ZEND_ENGINE_3
    case IS_BOOL:
      /* fallthrough */
#endif
    case IS_LONG:
      {
        zend_long loffset = (Z_TYPE_P(offset) == IS_DOUBLE) ? (zend_long)Z_DVAL_P(offset) : Z_LVAL_P(offset);
#ifdef ZEND_ENGINE_3
        if (zval *value = zend_hash_index_find(Z_ARRVAL_P(dict), loffset)) {
          ZVAL_ZVAL(return_value, value, 1, 0);
          return;
        }
#else
        zval **value;
        if (zend_hash_index_find(Z_ARRVAL_P(dict), loffset, (void **) &value) == SUCCESS) {
          ZVAL_ZVAL(return_value, *value, 1, 0);
          return;
        }
#endif
        zend_error(E_NOTICE, "Undefined offset:  %ld", loffset);
        RETURN_NULL();
      }

    case IS_STRING:
#ifdef ZEND_ENGINE_3
      if (zval *value = zend_symtable_find(Z_ARRVAL_P(dict), Z_STR_P(offset))) {
        ZVAL_ZVAL(return_value, value, 1, 0);
        return;
      }
#else
    {
      zval **value;
      if (zend_symtable_find(Z_ARRVAL_P(dict), Z_STRVAL_P(offset), Z_STRLEN_P(offset) + 1, (void **) &value) == SUCCESS) {
        ZVAL_ZVAL(return_value, *value, 1, 0);
        return;
      }
    }
#endif
      zend_error(E_NOTICE, "Undefined index:  %s", Z_STRVAL_P(offset));
      RETURN_NULL();

    default:
      zend_error(E_WARNING, "Illegal offset type");
      RETURN_NULL();
  }
}

//
// __xhp_idx
ZEND_FUNCTION(__xhp_idx) {
  zval *dict, *offset;
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zz", &dict, &offset) == FAILURE) {
    RETURN_NULL();
  }

  switch (Z_TYPE_P(dict)) {
    //
    // These are always NULL
    case IS_NULL:
#ifdef ZEND_ENGINE_3
    case IS_TRUE:
    case IS_FALSE:
#else
    case IS_BOOL:
#endif
    case IS_LONG:
    case IS_DOUBLE:
    default:
      RETURN_NULL();
      break;

    case IS_ARRAY:
      do_xhp_array(return_value, dict, offset TSRMLS_CC);
      return;

    //
    // 'string'[] -- String offset
    case IS_STRING: {
      long loffset;
      switch (Z_TYPE_P(offset)) {
#ifdef ZEND_ENGINE_3
        case IS_TRUE:  loffset = 1; break;
        case IS_FALSE: loffset = 0; break;
#else
        case IS_BOOL:  /* fallthrough */
#endif
        case IS_LONG:
          loffset = Z_LVAL_P(offset);
          break;

        case IS_DOUBLE:
          loffset = (long)Z_DVAL_P(offset);
          break;

        case IS_NULL:
          loffset = 0;
          break;

        case IS_STRING: {
          zval tmp = *offset;
          ZVAL_ZVAL(&tmp, offset, 1, 0);
          convert_to_long(&tmp);
          loffset = Z_LVAL(tmp);
          zval_dtor(&tmp);
          break;
        }

        default:
          zend_error(E_WARNING, "Illegal offset type");
          RETURN_NULL();
          break;
      }
      if (loffset < 0 || Z_STRLEN_P(dict) <= loffset) {
        zend_error(E_NOTICE, "Uninitialized string offset: %ld", loffset);
        RETURN_NULL();
      }
      XHP_ZVAL_STRINGL(return_value, Z_STRVAL_P(dict) + loffset, 1);
      return;
    }

    //
    // (new foo)[] -- Object overload (ArrayAccess)
    case IS_OBJECT:
      if (!Z_OBJ_HT_P(dict)->read_dimension) {
        zend_error(E_ERROR, "Cannot use object as array");
        RETURN_NULL();
      } else {
#ifdef ZEND_ENGINE_3
        zval rv;
        zval* overloaded_result = Z_OBJ_HT_P(dict)->read_dimension(dict, offset, BP_VAR_R, &rv);
#else
        zval* overloaded_result = Z_OBJ_HT_P(dict)->read_dimension(dict, offset, BP_VAR_R TSRMLS_CC);
#endif
        if (overloaded_result) {
          ZVAL_ZVAL(return_value, overloaded_result, 1, 1);
        } else {
          RETURN_NULL();
        }
      }
      break;
  }
}

//
// xhp_preprocess_code
ZEND_FUNCTION(xhp_preprocess_code) {
  // Parse zend params
  char *code;
  int code_len;
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &code, &code_len) == FAILURE) {
    RETURN_NULL();
  }
  string rewrit, error;
  uint32_t error_line;

  // Mangle code
  string code_str(code, code_len);
  XHPResult result = xhp_preprocess(code_str, rewrit, false, error, error_line);

  // Build return code
  array_init(return_value);
  if (result == XHPErred) {
    xhp_add_assoc_string(return_value, "error", const_cast<char*>(error.c_str()));
    add_assoc_long(return_value, "error_line", error_line);
  } else if (result == XHPRewrote) {
    xhp_add_assoc_string(return_value, "new_code", const_cast<char*>(rewrit.c_str()));
  }
}

//
// Module description
zend_function_entry xhp_functions[] = {
  ZEND_FE(__xhp_idx, NULL)
  ZEND_FE(xhp_preprocess_code, NULL)
  {NULL, NULL, NULL}
};

#if ZEND_MODULE_API_NO >= 20041225
static zend_module_dep xhp_module_deps[] = {
  /* Load any opcache first so that xhplib becomes the first pass */
  ZEND_MOD_OPTIONAL("apc")
  ZEND_MOD_OPTIONAL("Zend OPcache")
  { NULL, NULL, NULL }
};
#endif

zend_module_entry xhp_module_entry = {
#if ZEND_MODULE_API_NO >= 20041225
  STANDARD_MODULE_HEADER_EX, NULL,
  xhp_module_deps,
#else
  STANDARD_MODULE_HEADER,
#endif
  PHP_XHP_EXTNAME,
  xhp_functions,
  PHP_MINIT(xhp),
  PHP_MSHUTDOWN(xhp),
  NULL,
  NULL,
  PHP_MINFO(xhp),
  PHP_XHP_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_XHP
ZEND_GET_MODULE(xhp)
#endif
