/*
   +----------------------------------------------------------------------+
   | Zend OPcache                                                         |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2014 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Stanislav Malyshev <stas@zend.com>                          |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

#include "zend.h"
#include "ZendAccelerator.h"
#include "zend_persist.h"
#include "zend_extensions.h"
#include "zend_shared_alloc.h"
#include "zend_vm.h"
#include "zend_constants.h"
#include "zend_operators.h"

#define zend_accel_store(p, size) \
	    (p = _zend_shared_memdup((void*)p, size, 1))
#define zend_accel_memdup(p, size) \
	    _zend_shared_memdup((void*)p, size, 0)

#define zend_accel_store_string(str) do { \
		zend_string *new_str = zend_shared_alloc_get_xlat_entry(str); \
		if (new_str) { \
			zend_string_release(str); \
			str = new_str; \
		} else { \
	    	new_str = zend_accel_memdup((void*)str, _STR_HEADER_SIZE + (str)->len + 1); \
			zend_string_release(str); \
	    	str = new_str; \
	    	zend_string_hash_val(str); \
	    	GC_FLAGS(str) = IS_STR_INTERNED | IS_STR_PERMANENT; \
		} \
    } while (0)
#define zend_accel_memdup_string(str) do { \
		str = zend_accel_memdup(str, _STR_HEADER_SIZE + (str)->len + 1); \
    	zend_string_hash_val(str); \
		GC_FLAGS(str) = IS_STR_INTERNED | IS_STR_PERMANENT; \
	} while (0)
#define zend_accel_store_interned_string(str) do { \
		if (!IS_ACCEL_INTERNED(str)) { \
			zend_accel_store_string(str); \
		} \
	} while (0)
#define zend_accel_memdup_interned_string(str) do { \
		if (!IS_ACCEL_INTERNED(str)) { \
			zend_accel_memdup_string(str); \
		} \
	} while (0)

typedef void (*zend_persist_func_t)(zval*);

static void zend_persist_zval(zval *z);
static void zend_persist_zval_const(zval *z);

static const uint32_t uninitialized_bucket = {INVALID_IDX};

static void zend_hash_persist(HashTable *ht, zend_persist_func_t pPersistElement)
{
	uint idx;
	Bucket *p;

	if (!ht->nTableMask) {
		ht->arHash = (uint32_t*)&uninitialized_bucket;
		return;
	}
	if (ht->u.flags & HASH_FLAG_PACKED) {
		zend_accel_store(ht->arData, sizeof(Bucket) * ht->nNumUsed);
		ht->arHash = (uint32_t*)&uninitialized_bucket;
	} else {
		Bucket *d = (Bucket*)ZCG(mem);
		uint32_t *h = (uint32_t*)(d + ht->nNumUsed);

		ZCG(mem) = (void*)(h + ht->nTableSize);
		memcpy(d, ht->arData, sizeof(Bucket) * ht->nNumUsed);
		memcpy(h, ht->arHash, sizeof(uint32_t) * ht->nTableSize);
		efree(ht->arData);
		ht->arData = d;
		ht->arHash = h;
	}
	for (idx = 0; idx < ht->nNumUsed; idx++) {
		p = ht->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;

		/* persist bucket and key */
		if (p->key) {
			zend_accel_store_interned_string(p->key);
		}

		/* persist the data itself */
		pPersistElement(&p->val);
	}
}

static void zend_hash_persist_immutable(HashTable *ht)
{
	uint idx;
	Bucket *p;

	if (!ht->nTableMask) {
		ht->arHash = (uint32_t*)&uninitialized_bucket;
		return;
	}
	if (ht->u.flags & HASH_FLAG_PACKED) {
		ht->arData = zend_accel_memdup(ht->arData, sizeof(Bucket) * ht->nNumUsed);
		ht->arHash = (uint32_t*)&uninitialized_bucket;
	} else {
		Bucket *d = (Bucket*)ZCG(mem);
		uint32_t *h = (uint32_t*)(d + ht->nNumUsed);

		ZCG(mem) = (void*)(h + ht->nTableSize);
		memcpy(d, ht->arData, sizeof(Bucket) * ht->nNumUsed);
		memcpy(h, ht->arHash, sizeof(uint32_t) * ht->nTableSize);
		ht->arData = d;
		ht->arHash = h;
	}
	for (idx = 0; idx < ht->nNumUsed; idx++) {
		p = ht->arData + idx;
		if (Z_TYPE(p->val) == IS_UNDEF) continue;

		/* persist bucket and key */
		if (p->key) {
			zend_accel_memdup_interned_string(p->key);
		}

		/* persist the data itself */
		zend_persist_zval_const(&p->val);
	}
}

static zend_ast *zend_persist_ast(zend_ast *ast)
{
	uint32_t i;
	zend_ast *node;

	if (ast->kind == ZEND_AST_ZVAL) {
		zend_ast_zval *copy = zend_accel_memdup(ast, sizeof(zend_ast_zval));
		zend_persist_zval(&copy->val);
		node = (zend_ast *) copy;
	} else if (zend_ast_is_list(ast)) {
		zend_ast_list *list = zend_ast_get_list(ast);
		zend_ast_list *copy = zend_accel_memdup(ast,
			sizeof(zend_ast_list) - sizeof(zend_ast *) + sizeof(zend_ast *) * list->children);
		for (i = 0; i < list->children; i++) {
			if (copy->child[i]) {
				copy->child[i] = zend_persist_ast(copy->child[i]);
			}
		}
		node = (zend_ast *) copy;
	} else {
		uint32_t children = zend_ast_get_num_children(ast);
		node = zend_accel_memdup(ast, sizeof(zend_ast) - sizeof(zend_ast *) + sizeof(zend_ast *) * children);
		for (i = 0; i < children; i++) {
			if (node->child[i]) {
				node->child[i] = zend_persist_ast(node->child[i]);
			}
		}
	}

	efree(ast);
	return node;
}

static void zend_persist_zval(zval *z)
{
	zend_uchar flags;
	void *new_ptr;

	switch (Z_TYPE_P(z)) {
		case IS_STRING:
		case IS_CONSTANT:
			flags = Z_GC_FLAGS_P(z) & ~ (IS_STR_PERSISTENT | IS_STR_INTERNED | IS_STR_PERMANENT);
			zend_accel_store_interned_string(Z_STR_P(z));
			Z_GC_FLAGS_P(z) |= flags;
			Z_TYPE_FLAGS_P(z) &= ~(IS_TYPE_REFCOUNTED | IS_TYPE_COPYABLE);
			break;
		case IS_ARRAY:
			new_ptr = zend_shared_alloc_get_xlat_entry(Z_ARR_P(z));
			if (new_ptr) {
				Z_ARR_P(z) = new_ptr;
				Z_TYPE_FLAGS_P(z) = IS_TYPE_IMMUTABLE;
			} else {
				if (Z_IMMUTABLE_P(z)) {
					Z_ARR_P(z) = zend_accel_memdup(Z_ARR_P(z), sizeof(zend_array));
					zend_hash_persist_immutable(Z_ARRVAL_P(z));
				} else {
					GC_REMOVE_FROM_BUFFER(Z_ARR_P(z));
					zend_accel_store(Z_ARR_P(z), sizeof(zend_array));
					zend_hash_persist(Z_ARRVAL_P(z), zend_persist_zval);
					/* make immutable array */
					Z_TYPE_FLAGS_P(z) = IS_TYPE_IMMUTABLE;
					GC_REFCOUNT(Z_COUNTED_P(z)) = 2;
					Z_ARRVAL_P(z)->u.flags &= ~HASH_FLAG_APPLY_PROTECTION;
				}
			}
			break;
		case IS_REFERENCE:
			new_ptr = zend_shared_alloc_get_xlat_entry(Z_REF_P(z));
			if (new_ptr) {
				Z_REF_P(z) = new_ptr;
			} else {
				zend_accel_store(Z_REF_P(z), sizeof(zend_reference));
				zend_persist_zval(Z_REFVAL_P(z));
			}
			break;
		case IS_CONSTANT_AST:
			new_ptr = zend_shared_alloc_get_xlat_entry(Z_AST_P(z));
			if (new_ptr) {
				Z_AST_P(z) = new_ptr;
			} else {
				zend_accel_store(Z_AST_P(z), sizeof(zend_ast_ref));
				Z_ASTVAL_P(z) = zend_persist_ast(Z_ASTVAL_P(z));
			}
			break;
	}
}

static void zend_persist_zval_const(zval *z)
{
	zend_uchar flags;
	void *new_ptr;

	switch (Z_TYPE_P(z)) {
		case IS_STRING:
		case IS_CONSTANT:
			flags = Z_GC_FLAGS_P(z) & ~ (IS_STR_PERSISTENT | IS_STR_INTERNED | IS_STR_PERMANENT);
			zend_accel_memdup_interned_string(Z_STR_P(z));
			Z_GC_FLAGS_P(z) |= flags;
			Z_TYPE_FLAGS_P(z) &= ~(IS_TYPE_REFCOUNTED | IS_TYPE_COPYABLE);
			break;
		case IS_ARRAY:
			new_ptr = zend_shared_alloc_get_xlat_entry(Z_ARR_P(z));
			if (new_ptr) {
				Z_ARR_P(z) = new_ptr;
				Z_TYPE_FLAGS_P(z) = IS_TYPE_IMMUTABLE;
			} else {
				if (Z_IMMUTABLE_P(z)) {
					Z_ARR_P(z) = zend_accel_memdup(Z_ARR_P(z), sizeof(zend_array));
					zend_hash_persist_immutable(Z_ARRVAL_P(z));
				} else {
					GC_REMOVE_FROM_BUFFER(Z_ARR_P(z));
					zend_accel_store(Z_ARR_P(z), sizeof(zend_array));
					zend_hash_persist(Z_ARRVAL_P(z), zend_persist_zval);
					/* make immutable array */
					Z_TYPE_FLAGS_P(z) = IS_TYPE_IMMUTABLE;
					GC_REFCOUNT(Z_COUNTED_P(z)) = 2;
					Z_ARRVAL_P(z)->u.flags &= ~HASH_FLAG_APPLY_PROTECTION;
				}
			}
			break;
		case IS_REFERENCE:
			new_ptr = zend_shared_alloc_get_xlat_entry(Z_REF_P(z));
			if (new_ptr) {
				Z_REF_P(z) = new_ptr;
			} else {
				zend_accel_store(Z_REF_P(z), sizeof(zend_reference));
				zend_persist_zval(Z_REFVAL_P(z));
			}
			break;
		case IS_CONSTANT_AST:
			new_ptr = zend_shared_alloc_get_xlat_entry(Z_AST_P(z));
			if (new_ptr) {
				Z_AST_P(z) = new_ptr;
			} else {
				zend_accel_store(Z_AST_P(z), sizeof(zend_ast_ref));
				Z_ASTVAL_P(z) = zend_persist_ast(Z_ASTVAL_P(z));
			}
			break;
	}
}

static void zend_persist_op_array_ex(zend_op_array *op_array, zend_persistent_script* main_persistent_script)
{
	int already_stored = 0;
	zend_op *persist_ptr;
	zval *orig_literals = NULL;
	
	if (op_array->type != ZEND_USER_FUNCTION) {
		return;
	}

	if (--(*op_array->refcount) == 0) {
		efree(op_array->refcount);
	}
	op_array->refcount = NULL;

	if (main_persistent_script) {
		zend_execute_data *orig_execute_data = EG(current_execute_data);
		zend_execute_data fake_execute_data;
		zval *offset;

		memset(&fake_execute_data, 0, sizeof(fake_execute_data));
		fake_execute_data.func = (zend_function*)op_array;
		EG(current_execute_data) = &fake_execute_data;
		if ((offset = zend_get_constant_str("__COMPILER_HALT_OFFSET__", sizeof("__COMPILER_HALT_OFFSET__") - 1)) != NULL) {
			main_persistent_script->compiler_halt_offset = Z_LVAL_P(offset);
		}
		EG(current_execute_data) = orig_execute_data;
	}

	if (op_array->static_variables) {
		zend_hash_persist(op_array->static_variables, zend_persist_zval);
		zend_accel_store(op_array->static_variables, sizeof(HashTable));
	}

	if (zend_shared_alloc_get_xlat_entry(op_array->opcodes)) {
		already_stored = 1;
	}

	if (op_array->literals) {
		if (already_stored) {
			orig_literals = zend_shared_alloc_get_xlat_entry(op_array->literals);
			ZEND_ASSERT(orig_literals != NULL);
			op_array->literals = orig_literals;
		} else {
			zval *p = zend_accel_memdup(op_array->literals, sizeof(zval) * op_array->last_literal);
			zval *end = p + op_array->last_literal;
			orig_literals = op_array->literals;
			op_array->literals = p;
			while (p < end) {
				zend_persist_zval(p);
				p++;
			}
			efree(orig_literals);
		}
	}

	if (already_stored) {
		persist_ptr = zend_shared_alloc_get_xlat_entry(op_array->opcodes);
		ZEND_ASSERT(persist_ptr != NULL);
		op_array->opcodes = persist_ptr;
	} else {
		zend_op *new_opcodes = zend_accel_memdup(op_array->opcodes, sizeof(zend_op) * op_array->last);
#if ZEND_USE_ABS_CONST_ADDR || ZEND_USE_ABS_JMP_ADDR
		zend_op *opline = new_opcodes;
		zend_op *end = new_opcodes + op_array->last;
		int offset = 0;

		for (; opline < end ; opline++, offset++) {
# if ZEND_USE_ABS_CONST_ADDR
			if (ZEND_OP1_TYPE(opline) == IS_CONST) {
				opline->op1.zv = (zval*)((char*)opline->op1.zv + ((char*)op_array->literals - (char*)orig_literals));
			}
			if (ZEND_OP2_TYPE(opline) == IS_CONST) {
				opline->op2.zv = (zval*)((char*)opline->op2.zv + ((char*)op_array->literals - (char*)orig_literals));
			}
# endif
# if ZEND_USE_ABS_JMP_ADDR
			if (ZEND_DONE_PASS_TWO(op_array)) {
				/* fix jumps to point to new array */
				switch (opline->opcode) {
					case ZEND_JMP:
					case ZEND_GOTO:
					case ZEND_FAST_CALL:
						ZEND_OP1(opline).jmp_addr = &new_opcodes[ZEND_OP1(opline).jmp_addr - op_array->opcodes];
						break;
					case ZEND_JMPZNZ:
						/* relative extended_value don't have to be changed */
						/* break omitted intentionally */
					case ZEND_JMPZ:
					case ZEND_JMPNZ:
					case ZEND_JMPZ_EX:
					case ZEND_JMPNZ_EX:
					case ZEND_JMP_SET:
					case ZEND_COALESCE:
					case ZEND_NEW:
					case ZEND_FE_RESET:
					case ZEND_FE_FETCH:
						ZEND_OP2(opline).jmp_addr = &new_opcodes[ZEND_OP2(opline).jmp_addr - op_array->opcodes];
						break;
				}
			}
# endif
		}
#endif

		efree(op_array->opcodes);
		op_array->opcodes = new_opcodes;

		if (op_array->run_time_cache) {
			efree(op_array->run_time_cache);
			op_array->run_time_cache = NULL;
		}
	}

	if (op_array->function_name && !IS_ACCEL_INTERNED(op_array->function_name)) {
		zend_string *new_name;
		if (already_stored) {
			new_name = zend_shared_alloc_get_xlat_entry(op_array->function_name);
			ZEND_ASSERT(new_name != NULL);
			op_array->function_name = new_name;
		} else {
			zend_accel_store_string(op_array->function_name);
		}
	}

	if (op_array->filename) {
		/* do not free! PHP has centralized filename storage, compiler will free it */
		zend_accel_memdup_string(op_array->filename);
	}

	if (op_array->arg_info) {
		if (already_stored) {
			zend_arg_info *new_ptr = zend_shared_alloc_get_xlat_entry(op_array->arg_info);
			ZEND_ASSERT(new_ptr != NULL);
			op_array->arg_info = new_ptr;
		} else {
			uint32_t i;

			zend_accel_store(op_array->arg_info, sizeof(zend_arg_info) * op_array->num_args);
			for (i = 0; i < op_array->num_args; i++) {
				if (op_array->arg_info[i].name) {
					zend_accel_store_interned_string(op_array->arg_info[i].name);
				}
				if (op_array->arg_info[i].class_name) {
					zend_accel_store_interned_string(op_array->arg_info[i].class_name);
				}
			}
		}
	}

	if (op_array->brk_cont_array) {
		zend_accel_store(op_array->brk_cont_array, sizeof(zend_brk_cont_element) * op_array->last_brk_cont);
	}

	if (op_array->scope) {
		op_array->scope = zend_shared_alloc_get_xlat_entry(op_array->scope);
	}

	if (op_array->doc_comment) {
		if (ZCG(accel_directives).save_comments) {
			if (already_stored) {
				op_array->doc_comment = zend_shared_alloc_get_xlat_entry(op_array->doc_comment);
				ZEND_ASSERT(op_array->doc_comment != NULL);
			} else {
				zend_accel_store_string(op_array->doc_comment);
			}
		} else {
			if (!already_stored) {
				zend_string_release(op_array->doc_comment);
			}
			op_array->doc_comment = NULL;
		}
	}

	if (op_array->try_catch_array) {
		zend_accel_store(op_array->try_catch_array, sizeof(zend_try_catch_element) * op_array->last_try_catch);
	}

	if (op_array->vars) {
		if (already_stored) {
			persist_ptr = zend_shared_alloc_get_xlat_entry(op_array->vars);
			ZEND_ASSERT(persist_ptr != NULL);
			op_array->vars = (zend_string**)persist_ptr;
		} else {
			int i;
			zend_accel_store(op_array->vars, sizeof(zend_string*) * op_array->last_var);
			for (i = 0; i < op_array->last_var; i++) {
				zend_accel_store_interned_string(op_array->vars[i]);
			}
		}
	}

	/* "prototype" may be undefined if "scope" isn't set */
	if (op_array->scope && op_array->prototype) {
		if ((persist_ptr = zend_shared_alloc_get_xlat_entry(op_array->prototype))) {
			op_array->prototype = (union _zend_function*)persist_ptr;
			/* we use refcount to show that op_array is referenced from several places */
			op_array->prototype->op_array.refcount++;
		}
	} else {
		op_array->prototype = NULL;
	}
}

static void zend_persist_op_array(zval *zv)
{
	memcpy(ZCG(arena_mem), Z_PTR_P(zv), sizeof(zend_op_array));
	zend_shared_alloc_register_xlat_entry(Z_PTR_P(zv), ZCG(arena_mem));
	Z_PTR_P(zv) = ZCG(arena_mem);
	ZCG(arena_mem) = (void*)((char*)ZCG(arena_mem) + ZEND_ALIGNED_SIZE(sizeof(zend_op_array)));
	zend_persist_op_array_ex(Z_PTR_P(zv), NULL);
}

static void zend_persist_property_info(zval *zv)
{
	zend_property_info *prop;

	memcpy(ZCG(arena_mem), Z_PTR_P(zv), sizeof(zend_property_info));
	zend_shared_alloc_register_xlat_entry(Z_PTR_P(zv), ZCG(arena_mem));
	prop = Z_PTR_P(zv) = ZCG(arena_mem);
	ZCG(arena_mem) = (void*)((char*)ZCG(arena_mem) + ZEND_ALIGNED_SIZE(sizeof(zend_property_info)));
	zend_accel_store_interned_string(prop->name);
	if (prop->doc_comment) {
		if (ZCG(accel_directives).save_comments) {
			zend_accel_store_string(prop->doc_comment);
		} else {
			if (!zend_shared_alloc_get_xlat_entry(prop->doc_comment)) {
				zend_shared_alloc_register_xlat_entry(prop->doc_comment, prop->doc_comment);
			}
			zend_string_release(prop->doc_comment);
			prop->doc_comment = NULL;
		}
	}
}

static void zend_persist_class_entry(zval *zv)
{
	zend_class_entry *ce = Z_PTR_P(zv);

	if (ce->type == ZEND_USER_CLASS) {
		memcpy(ZCG(arena_mem), Z_PTR_P(zv), sizeof(zend_class_entry));
		zend_shared_alloc_register_xlat_entry(Z_PTR_P(zv), ZCG(arena_mem));
		ce = Z_PTR_P(zv) = ZCG(arena_mem);
		ZCG(arena_mem) = (void*)((char*)ZCG(arena_mem) + ZEND_ALIGNED_SIZE(sizeof(zend_class_entry)));
		zend_accel_store_interned_string(ce->name);
		zend_hash_persist(&ce->function_table, zend_persist_op_array);
		if (ce->default_properties_table) {
		    int i;

			zend_accel_store(ce->default_properties_table, sizeof(zval) * ce->default_properties_count);
			for (i = 0; i < ce->default_properties_count; i++) {
				zend_persist_zval(&ce->default_properties_table[i]);
			}
		}
		if (ce->default_static_members_table) {
		    int i;

			zend_accel_store(ce->default_static_members_table, sizeof(zval) * ce->default_static_members_count);
			for (i = 0; i < ce->default_static_members_count; i++) {
				zend_persist_zval(&ce->default_static_members_table[i]);
			}
		}
		ce->static_members_table = NULL;

		zend_hash_persist(&ce->constants_table, zend_persist_zval);

		if (ZEND_CE_FILENAME(ce)) {
			/* do not free! PHP has centralized filename storage, compiler will free it */
			zend_accel_memdup_string(ZEND_CE_FILENAME(ce));
		}
		if (ZEND_CE_DOC_COMMENT(ce)) {
			if (ZCG(accel_directives).save_comments) {
				zend_accel_store_string(ZEND_CE_DOC_COMMENT(ce));
			} else {
				if (!zend_shared_alloc_get_xlat_entry(ZEND_CE_DOC_COMMENT(ce))) {
					zend_shared_alloc_register_xlat_entry(ZEND_CE_DOC_COMMENT(ce), ZEND_CE_DOC_COMMENT(ce));
					zend_string_release(ZEND_CE_DOC_COMMENT(ce));
				}
				ZEND_CE_DOC_COMMENT(ce) = NULL;
			}
		}
		zend_hash_persist(&ce->properties_info, zend_persist_property_info);
		if (ce->num_interfaces && ce->interfaces) {
			efree(ce->interfaces);
		}
		ce->interfaces = NULL; /* will be filled in on fetch */

		if (ce->num_traits && ce->traits) {
			efree(ce->traits);
		}
		ce->traits = NULL;

		if (ce->trait_aliases) {
			int i = 0;
			while (ce->trait_aliases[i]) {
				if (ce->trait_aliases[i]->trait_method) {
					if (ce->trait_aliases[i]->trait_method->method_name) {
						zend_accel_store_interned_string(ce->trait_aliases[i]->trait_method->method_name);
					}
					if (ce->trait_aliases[i]->trait_method->class_name) {
						zend_accel_store_interned_string(ce->trait_aliases[i]->trait_method->class_name);
					}
					ce->trait_aliases[i]->trait_method->ce = NULL;
					zend_accel_store(ce->trait_aliases[i]->trait_method,
						sizeof(zend_trait_method_reference));
				}

				if (ce->trait_aliases[i]->alias) {
					zend_accel_store_interned_string(ce->trait_aliases[i]->alias);
				}

				zend_accel_store(ce->trait_aliases[i], sizeof(zend_trait_alias));
				i++;
			}

			zend_accel_store(ce->trait_aliases, sizeof(zend_trait_alias*) * (i + 1));
		}

		if (ce->trait_precedences) {
			int i = 0;

			while (ce->trait_precedences[i]) {
				zend_accel_store_interned_string(ce->trait_precedences[i]->trait_method->method_name);
				zend_accel_store_interned_string(ce->trait_precedences[i]->trait_method->class_name);
				ce->trait_precedences[i]->trait_method->ce = NULL;
				zend_accel_store(ce->trait_precedences[i]->trait_method,
					sizeof(zend_trait_method_reference));

				if (ce->trait_precedences[i]->exclude_from_classes) {
					int j = 0;

					while (ce->trait_precedences[i]->exclude_from_classes[j].class_name) {
						zend_accel_store_interned_string(ce->trait_precedences[i]->exclude_from_classes[j].class_name);
						j++;
					}
					zend_accel_store(ce->trait_precedences[i]->exclude_from_classes,
						sizeof(zend_class_entry*) * (j + 1));
				}

				zend_accel_store(ce->trait_precedences[i], sizeof(zend_trait_precedence));
				i++;
			}
			zend_accel_store(
				ce->trait_precedences, sizeof(zend_trait_precedence*) * (i + 1));
		}
	}
}

static int zend_update_property_info_ce(zval *zv)
{
	zend_property_info *prop = Z_PTR_P(zv);

	prop->ce = zend_shared_alloc_get_xlat_entry(prop->ce);
	return 0;
}

static int zend_update_parent_ce(zval *zv)
{
	zend_class_entry *ce = Z_PTR_P(zv);

	if (ce->parent) {
		ce->parent = zend_shared_alloc_get_xlat_entry(ce->parent);
		/* We use refcount to show if the class is used as a parent */
		ce->parent->refcount++;
	}

	/* update methods */
	if (ce->constructor) {
		ce->constructor = zend_shared_alloc_get_xlat_entry(ce->constructor);
		/* we use refcount to show that op_array is referenced from several places */
		ce->constructor->op_array.refcount++;
	}
	if (ce->destructor) {
		ce->destructor = zend_shared_alloc_get_xlat_entry(ce->destructor);
		ce->destructor->op_array.refcount++;
	}
	if (ce->clone) {
		ce->clone = zend_shared_alloc_get_xlat_entry(ce->clone);
		ce->clone->op_array.refcount++;
	}
	if (ce->__get) {
		ce->__get = zend_shared_alloc_get_xlat_entry(ce->__get);
		ce->__get->op_array.refcount++;
	}
	if (ce->__set) {
		ce->__set = zend_shared_alloc_get_xlat_entry(ce->__set);
		ce->__set->op_array.refcount++;
	}
	if (ce->__call) {
		ce->__call = zend_shared_alloc_get_xlat_entry(ce->__call);
		ce->__call->op_array.refcount++;
	}
	if (ce->serialize_func) {
		ce->serialize_func = zend_shared_alloc_get_xlat_entry(ce->serialize_func);
		ce->serialize_func->op_array.refcount++;
	}
	if (ce->unserialize_func) {
		ce->unserialize_func = zend_shared_alloc_get_xlat_entry(ce->unserialize_func);
		ce->unserialize_func->op_array.refcount++;
	}
	if (ce->__isset) {
		ce->__isset = zend_shared_alloc_get_xlat_entry(ce->__isset);
		ce->__isset->op_array.refcount++;
	}
	if (ce->__unset) {
		ce->__unset = zend_shared_alloc_get_xlat_entry(ce->__unset);
		ce->__unset->op_array.refcount++;
	}
	if (ce->__tostring) {
		ce->__tostring = zend_shared_alloc_get_xlat_entry(ce->__tostring);
		ce->__tostring->op_array.refcount++;
	}
	if (ce->__callstatic) {
		ce->__callstatic = zend_shared_alloc_get_xlat_entry(ce->__callstatic);
		ce->__callstatic->op_array.refcount++;
	}
	if (ce->__debugInfo) {
		ce->__debugInfo = zend_shared_alloc_get_xlat_entry(ce->__debugInfo);
		ce->__debugInfo->op_array.refcount++;
	}
	zend_hash_apply(&ce->properties_info, (apply_func_t) zend_update_property_info_ce);
	return 0;
}

static void zend_accel_persist_class_table(HashTable *class_table)
{
    zend_hash_persist(class_table, zend_persist_class_entry);
	zend_hash_apply(class_table, (apply_func_t) zend_update_parent_ce);
}

zend_persistent_script *zend_accel_script_persist(zend_persistent_script *script, char **key, unsigned int key_length)
{
	script->mem = ZCG(mem);

	zend_shared_alloc_clear_xlat_table();
	
	zend_accel_store(script, sizeof(zend_persistent_script));
	*key = zend_accel_memdup(*key, key_length + 1);
	zend_accel_store_string(script->full_path);

	script->arena_mem = ZCG(arena_mem) = ZCG(mem);
	ZCG(mem) = (void*)((char*)ZCG(mem) + script->arena_size);

	zend_accel_persist_class_table(&script->class_table);
	zend_hash_persist(&script->function_table, zend_persist_op_array);
	zend_persist_op_array_ex(&script->main_op_array, script);

	return script;
}

int zend_accel_script_persistable(zend_persistent_script *script)
{
	return 1;
}
