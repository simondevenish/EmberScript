// compiler.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "virtual_machine.h"
#include "parser.h"  // For ASTNodeType, ASTNode, etc.
#include "utils.h"

static void compile_node(ASTNode* node, BytecodeChunk* chunk, SymbolTable* symtab);

/* -------------------------------------------------------
   Symbol Table Implementation
   ------------------------------------------------------- */
SymbolTable* symbol_table_create() {
    SymbolTable* table = (SymbolTable*)malloc(sizeof(SymbolTable));
    if (!table) return NULL;
    table->symbols = NULL;
    table->capacity = 0;
    table->count = 0;
    return table;
}

void symbol_table_free(SymbolTable* table) {
    if (!table) return;
    for (int i = 0; i < table->count; i++) {
        free(table->symbols[i].name);
    }
    free(table->symbols);
    free(table);
}

static void ensure_symtab_capacity(SymbolTable* table) {
    if (table->count >= table->capacity) {
        int new_capacity = (table->capacity < 8) ? 8 : table->capacity * 2;
        Symbol* new_symbols = realloc(table->symbols, new_capacity * sizeof(Symbol));
        if (!new_symbols) {
            fprintf(stderr, "Error: SymbolTable reallocation failed.\n");
            exit(EXIT_FAILURE);
        }
        table->symbols = new_symbols;
        table->capacity = new_capacity;
    }
}

int symbol_table_get_or_add(SymbolTable* table, const char* name, bool isFunction) {
    // See if the symbol already exists
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->symbols[i].name, name) == 0) {
            // If we want to differentiate variable vs. function, we could check isFunction
            return table->symbols[i].index;
        }
    }
    // Otherwise, create a new symbol
    ensure_symtab_capacity(table);
    int index = table->count;
    table->symbols[index].name = strdup(name);
    table->symbols[index].index = index;      // For simplicity
    table->symbols[index].isFunction = isFunction;
    table->count++;
    return index;
}

/* -------------------------------------------------------
   Utility: Emit Single Byte or Byte + Operand
   ------------------------------------------------------- */
static void emit_byte(BytecodeChunk* chunk, uint8_t byte) {
    vm_chunk_write_byte(chunk, byte);
}
static void emit_two_bytes(BytecodeChunk* chunk, uint8_t b1, uint8_t b2) {
    emit_byte(chunk, b1);
    emit_byte(chunk, b2);
}

static int emit_jump(BytecodeChunk* chunk, uint8_t jumpOp) {
    // Emit the jump opcode plus two bytes for the jump offset (16-bit, big-endian)
    emit_byte(chunk, jumpOp);
    emit_byte(chunk, 0xFF); // placeholder high
    emit_byte(chunk, 0xFF); // placeholder low
    // Return the position of the offset
    return chunk->code_count - 2; 
}

static void patch_jump(BytecodeChunk* chunk, int offset) {
    // Calculate how far to jump from “offset” to the end of the chunk
    int jump_distance = chunk->code_count - offset - 2; 
    // Overwrite the placeholder bytes
    chunk->code[offset]   = (jump_distance >> 8) & 0xFF;
    chunk->code[offset+1] = jump_distance & 0xFF;
}

static int add_constant(BytecodeChunk* chunk, RuntimeValue val) {
    return vm_chunk_add_constant(chunk, val);
}
static void emit_constant(BytecodeChunk* chunk, RuntimeValue val) {
    int index = add_constant(chunk, val);
    emit_byte(chunk, OP_LOAD_CONST);
    emit_byte(chunk, (uint8_t)index);
}

/* -------------------------------------------------------
   Expression Compiler
   ------------------------------------------------------- */
static void compile_expression(ASTNode* node, BytecodeChunk* chunk, SymbolTable* symtab) {
    switch (node->type) {
        case AST_LITERAL: {
            RuntimeValue cval;
            memset(&cval, 0, sizeof(cval));
            switch (node->literal.token_type) {
                case TOKEN_NUMBER:
                    cval.type = RUNTIME_VALUE_NUMBER;
                    cval.number_value = atof(node->literal.value);
                    break;
                case TOKEN_STRING:
                    cval.type = RUNTIME_VALUE_STRING;
                    cval.string_value = strdup(node->literal.value);
                    break;
                case TOKEN_BOOLEAN:
                    cval.type = RUNTIME_VALUE_BOOLEAN;
                    cval.boolean_value = (strcmp(node->literal.value, "true") == 0);
                    break;
                case TOKEN_NULL:
                    cval.type = RUNTIME_VALUE_NULL;
                    break;
                default:
                    fprintf(stderr, "Compiler error: Unrecognized literal.\n");
                    cval.type = RUNTIME_VALUE_NULL;
            }
            emit_constant(chunk, cval);
            break;
        }
        case AST_VARIABLE: {
            // Load from variable
            int varIndex = symbol_table_get_or_add(symtab, node->variable.variable_name, false);
            emit_byte(chunk, OP_LOAD_VAR);
            emit_byte(chunk, (uint8_t)varIndex);
            break;
        }
        case AST_ASSIGNMENT: {
            // compile right-hand side
            compile_expression(node->assignment.value, chunk, symtab);
            // store into variable
            int varIndex = symbol_table_get_or_add(symtab, node->assignment.variable, false);
            emit_byte(chunk, OP_STORE_VAR);
            emit_byte(chunk, (uint8_t)varIndex);
            // The value remains on stack (if you want the assignment to produce a value).
            break;
        }
        case AST_BINARY_OP: {
            // compile left, then right
            compile_expression(node->binary_op.left, chunk, symtab);
            compile_expression(node->binary_op.right, chunk, symtab);
            // pick an opcode
            const char* op = node->binary_op.op_symbol;
            if (strcmp(op, "+") == 0) {
                emit_byte(chunk, OP_ADD);
            } else if (strcmp(op, "-") == 0) {
                emit_byte(chunk, OP_SUB);
            } else if (strcmp(op, "*") == 0) {
                emit_byte(chunk, OP_MUL);
            } else if (strcmp(op, "/") == 0) {
                emit_byte(chunk, OP_DIV);
            } else if (strcmp(op, "==") == 0) {
                emit_byte(chunk, OP_EQ);
            } else if (strcmp(op, "!=") == 0) {
                emit_byte(chunk, OP_NEQ);
            } else if (strcmp(op, "<") == 0) {
                emit_byte(chunk, OP_LT);
            } else if (strcmp(op, ">") == 0) {
                emit_byte(chunk, OP_GT);
            } else if (strcmp(op, "<=") == 0) {
                emit_byte(chunk, OP_LTE);
            } else if (strcmp(op, ">=") == 0) {
                emit_byte(chunk, OP_GTE);
            } else {
                fprintf(stderr, "Compiler error: Unsupported binary operator '%s'\n", op);
            }
            break;
        }
        case AST_FUNCTION_CALL: {
            // Special-case “print(…)" as a builtin
            // TODO(SD) this is an example placeholder
            if (strcmp(node->function_call.function_name, "print") == 0) {
                // Evaluate the arguments (for adventure_game.ember, we assume 1 arg)
                for (int i = 0; i < node->function_call.argument_count; i++) {
                    compile_expression(node->function_call.arguments[i], chunk, symtab);
                }
                // OP_PRINT
                emit_byte(chunk, OP_PRINT);
            } else {
                // For user-defined function calls:
                //  1) push arguments (left->right)
                for (int i = 0; i < node->function_call.argument_count; i++) {
                    compile_expression(node->function_call.arguments[i], chunk, symtab);
                }
                //  2) Identify function (store index in constant or symbol table)
                int funcIndex = symbol_table_get_or_add(symtab, node->function_call.function_name, true);
                //  3) OP_CALL <funcIndex> <argCount>
                emit_byte(chunk, OP_CALL);
                emit_byte(chunk, (uint8_t)funcIndex);
                emit_byte(chunk, (uint8_t)node->function_call.argument_count);
            }
            break;
        }
        case AST_ARRAY_LITERAL: {
            // For minimal array support: create a new array, push elements
            // OP_NEW_ARRAY, then OP_ARRAY_PUSH for each element
            int count = node->array_literal.element_count;
            emit_byte(chunk, OP_NEW_ARRAY);
            // Now stack has a new empty array
            for (int i = 0; i < count; i++) {
                // push the array ref again
                emit_byte(chunk, OP_DUP);
                // compile the element
                compile_expression(node->array_literal.elements[i], chunk, symtab);
                // OP_ARRAY_PUSH
                emit_byte(chunk, OP_ARRAY_PUSH);
            }
            // The resulting array is on the stack top
            break;
        }
        case AST_INDEX_ACCESS: {
            // compile array expr
            compile_expression(node->index_access.array_expr, chunk, symtab);
            // compile index
            compile_expression(node->index_access.index_expr, chunk, symtab);
            // OP_GET_INDEX
            emit_byte(chunk, OP_GET_INDEX);
            break;
        }
        case AST_UNARY_OP: {
            // e.g. !x
            compile_expression(node->unary_op.operand, chunk, symtab);
            if (strcmp(node->unary_op.op_symbol, "!") == 0) {
                emit_byte(chunk, OP_NOT);
            } else if (strcmp(node->unary_op.op_symbol, "-") == 0) {
                emit_byte(chunk, OP_NEG);
            } else {
                fprintf(stderr, "Compiler error: Unknown unary op '%s'\n", node->unary_op.op_symbol);
            }
            break;
        }
        default:
            // If we see a statement node in an expression context, that’s likely a parse mismatch
            fprintf(stderr, "Compiler error: Unexpected node type %d in expression.\n", node->type);
            break;
    }
}

/* -------------------------------------------------------
   Statement Compiler
   ------------------------------------------------------- */
static void compile_statement(ASTNode* node, BytecodeChunk* chunk, SymbolTable* symtab) {
    switch (node->type) {
        case AST_VARIABLE_DECL: {
            // var X = <expr>;
            if (node->variable_decl.initial_value) {
                compile_expression(node->variable_decl.initial_value, chunk, symtab);
            } else {
                // No initializer => push null
                RuntimeValue cval;
                cval.type = RUNTIME_VALUE_NULL;
                emit_constant(chunk, cval);
            }
            int varIndex = symbol_table_get_or_add(symtab, node->variable_decl.variable_name, false);
            emit_byte(chunk, OP_STORE_VAR);
            emit_byte(chunk, (uint8_t)varIndex);
            break;
        }
        case AST_ASSIGNMENT:
        case AST_BINARY_OP:
        case AST_FUNCTION_CALL:
        case AST_ARRAY_LITERAL:
        case AST_INDEX_ACCESS:
        case AST_UNARY_OP:
        case AST_LITERAL:
        case AST_VARIABLE: {
            // Expression statement
            compile_expression(node, chunk, symtab);
            // pop result (unless we want to keep it)
            emit_byte(chunk, OP_POP);
            break;
        }
        case AST_IF_STATEMENT: {
            // if (cond) { body } else { elseBody }
            // compile condition
            compile_expression(node->if_statement.condition, chunk, symtab);
            // Jump if false => else part
            int elseJump = emit_jump(chunk, OP_JUMP_IF_FALSE);
            // compile if body
            compile_node(node->if_statement.body, chunk, symtab);
            // jump over else
            int endJump = emit_jump(chunk, OP_JUMP);
            // patch else jump
            patch_jump(chunk, elseJump);
            // compile else if it exists
            if (node->if_statement.else_body) {
                compile_node(node->if_statement.else_body, chunk, symtab);
            }
            // patch end jump
            patch_jump(chunk, endJump);
            break;
        }
        case AST_WHILE_LOOP: {
            // while (cond) { body }
            // label loopStart
            int loopStart = chunk->code_count;
            // compile cond
            compile_expression(node->while_loop.condition, chunk, symtab);
            // jump if false => loopEnd
            int loopEndJump = emit_jump(chunk, OP_JUMP_IF_FALSE);
            // compile body
            compile_node(node->while_loop.body, chunk, symtab);
            // jump back to loopStart
            emit_byte(chunk, OP_LOOP);
            // We store a 2-byte offset for OP_LOOP
            // Distance = current - loopStart + 2 (the size of OP_LOOP itself)
            int offset = chunk->code_count - loopStart + 2;
            // High byte
            emit_byte(chunk, (offset >> 8) & 0xFF);
            // Low byte
            emit_byte(chunk, offset & 0xFF);

            // patch loopEnd
            patch_jump(chunk, loopEndJump);
            break;
        }
        case AST_IMPORT: {
            const char* filename = node->import_stmt.import_path;

            // 1) Read file
            char* import_source = read_file(filename);
            if (!import_source) {
                fprintf(stderr, "Compiler error: Could not open import file '%s'\n", filename);
                return;
            }

            // 2) Lex & parse => build an AST
            Lexer import_lexer;
            lexer_init(&import_lexer, import_source);
            Parser* import_parser = parser_create(&import_lexer);
            ASTNode* import_root = parse_script(import_parser);

            if (!import_root) {
                fprintf(stderr, "Compiler error: Parsing '%s' failed.\n", filename);
                free(import_parser);
                free(import_source);
                return;
            }
            
            // 3) Compile the new AST into *this same* chunk + symtab
            bool ok = compile_ast(import_root, chunk, symtab);
            if (!ok) {
                fprintf(stderr, "Compiler error: Sub-compile for '%s' failed.\n", filename);
            }

            // 4) **Remove final OP_EOF** if present
            if (chunk->code_count > 0 &&
                chunk->code[chunk->code_count - 1] == OP_EOF)
            {
                chunk->code_count--;
            }

            // 5) Cleanup
            free_ast(import_root);
            free(import_parser);
            free(import_source);

            // no code needed at runtime => we just physically merged it
            break;
        }

        case AST_FOR_LOOP: {
            // for (init; cond; inc) { body }
            // compile init
            if (node->for_loop.initializer) {
                compile_node(node->for_loop.initializer, chunk, symtab);
            }
            // label loopStart
            int loopStart = chunk->code_count;
            // compile cond
            if (node->for_loop.condition) {
                compile_expression(node->for_loop.condition, chunk, symtab);
            } else {
                // If no condition, we treat it as 'true'
                RuntimeValue cval;
                cval.type = RUNTIME_VALUE_BOOLEAN;
                cval.boolean_value = true;
                emit_constant(chunk, cval);
            }
            // jump if false => loopEnd
            int loopEndJump = emit_jump(chunk, OP_JUMP_IF_FALSE);

            // compile body
            compile_node(node->for_loop.body, chunk, symtab);

            // compile inc
            if (node->for_loop.increment) {
                compile_expression(node->for_loop.increment, chunk, symtab);
                emit_byte(chunk, OP_POP); // discard inc result
            }
            // jump back to loopStart
            emit_byte(chunk, OP_LOOP);
            int offset = chunk->code_count - loopStart + 2;
            emit_byte(chunk, (offset >> 8) & 0xFF);
            emit_byte(chunk, offset & 0xFF);

            // patch loopEnd
            patch_jump(chunk, loopEndJump);
            break;
        }
        case AST_FUNCTION_DEF: {
            // A minimal approach:
            //   1) We skip emitting code for the body right now, OR
            //   2) We compile the function body into the same chunk at some label,
            //      store that label in a constant or symbol table, then store it in a variable.
            //
            // For adventure_game.ember, we _do_ have function definitions. Let's store them as
            // “functionName -> instruction pointer offset” in the symbol table. We'll place
            // them in a separate “jump over function” block for now.
            //
            // For simplicity, we’ll do nothing but store a “null” if we just want to parse it
            // without actually using VM calls. The example below does a partial approach.
            //
            // Better approach is: 
            //   - Record the current IP
            //   - compile the function body (like a mini chunk)
            //   - store function meta in the symtab
            //   - when calling, jump to that IP, then jump back
            // But that’s more advanced than we want for the first pass. 
            //
            // Let's do a do-nothing placeholder so we can parse the script successfully:
            int funcIndex = symbol_table_get_or_add(symtab, node->function_def.function_name, true);
            // We won't generate real code for the function body. So let's just ignore parameters & body:
            break;
        }
        case AST_BLOCK: {
            // compile each statement
            for (int i = 0; i < node->block.statement_count; i++) {
                compile_node(node->block.statements[i], chunk, symtab);
            }
            break;
        }
        case AST_SWITCH_CASE: {
            // (Placeholder) We have not implemented codegen for switch statements yet.
            // For now, do nothing or produce a warning.
            // e.g.,
            fprintf(stderr, "Warning: Switch/case code generation not implemented.\n");
            break;
        }
        default:
            fprintf(stderr, "Compiler error: Unhandled statement node type %d\n", node->type);
            break;
    }
}

/* -------------------------------------------------------
   Recursively compile an AST node
   ------------------------------------------------------- */
static void compile_node(ASTNode* node, BytecodeChunk* chunk, SymbolTable* symtab) {
    if (!node) return;

    switch (node->type) {
        // Anything that can appear as a top-level statement
        case AST_VARIABLE_DECL:
        case AST_ASSIGNMENT:
        case AST_FUNCTION_CALL:
        case AST_IF_STATEMENT:
        case AST_WHILE_LOOP:
        case AST_FOR_LOOP:
        case AST_FUNCTION_DEF:
        case AST_BLOCK:
        case AST_BINARY_OP:
        case AST_ARRAY_LITERAL:
        case AST_INDEX_ACCESS:
        case AST_UNARY_OP:
        case AST_LITERAL:
        case AST_VARIABLE:
        case AST_IMPORT:
        case AST_SWITCH_CASE:
            compile_statement(node, chunk, symtab);
            break;

        default:
            fprintf(stderr, "Compiler error: compile_node unrecognized AST type %d.\n", node->type);
            break;
    }
}

/* -------------------------------------------------------
   Main Entry Point
   ------------------------------------------------------- */
bool compile_ast(ASTNode* ast, BytecodeChunk* chunk, SymbolTable* symtab) {
    if (!ast || !chunk || !symtab) {
        fprintf(stderr, "Error: compile_ast called with invalid arguments.\n");
        return false;
    }

    compile_node(ast, chunk, symtab);

    // Finally, emit an OP_EOF or OP_RETURN to cleanly end
    emit_byte(chunk, OP_EOF);
    return true;
}
