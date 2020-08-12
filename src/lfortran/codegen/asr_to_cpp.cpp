#include <iostream>
#include <memory>

#include <lfortran/asr.h>
#include <lfortran/containers.h>
#include <lfortran/codegen/asr_to_cpp.h>
#include <lfortran/exception.h>
#include <lfortran/asr_utils.h>


namespace LFortran {

// Platform dependent fast unique hash:
uint64_t get_hash(ASR::asr_t *node)
{
    return (uint64_t)node;
}

struct SymbolInfo
{
    bool needs_declaration = true;
    bool intrinsic_function = false;
};

std::string convert_variable_decl(const ASR::Variable_t &v)
{
    std::string sub;
    bool use_ref = (v.m_intent == intent_out || v.m_intent == intent_inout);
    if (v.m_type->type == ASR::ttypeType::Integer) {
        std::string ref;
        if (use_ref) ref = "&";
        sub += "int " + ref + std::string(v.m_name);
    } else if (v.m_type->type == ASR::ttypeType::Real) {
        ASR::Real_t *t = TYPE_REAL((ASR::asr_t*)v.m_type);
        std::string dims;
        for (size_t i=0; i<t->n_dims; i++) {
            ASR::expr_t *start = t->m_dims[i].m_start;
            ASR::expr_t *end = t->m_dims[i].m_end;
            if (!start && !end) {
                dims += "*";
            } else {
                throw CodeGenError("Dimension type not supported");
            }
        }
        if (t->n_dims == 0) {
            std::string ref;
            if (use_ref) ref = "&";
            sub += "float " + ref + std::string(v.m_name);
        } else {
            std::string c;
            if (!use_ref) c = "const ";
            sub += "const Kokkos::View<" + c + "float" + dims + "> &" + std::string(v.m_name);
        }
    } else if (v.m_type->type == ASR::ttypeType::Logical) {
        std::string ref;
        if (use_ref) ref = "&";
        sub += "bool " + ref + std::string(v.m_name);
    } else {
        throw CodeGenError("Type not supported");
    }
    return sub;
}

class ASRToCPPVisitor : public ASR::BaseVisitor<ASRToCPPVisitor>
{
public:
    std::map<uint64_t, SymbolInfo> sym_info;
    std::string src;
    int indentation_level;
    int indentation_spaces;
    bool last_unary_plus;
    bool last_binary_plus;

    void visit_TranslationUnit(const ASR::TranslationUnit_t &x) {
        // All loose statements must be converted to a function, so the items
        // must be empty:
        LFORTRAN_ASSERT(x.n_items == 0);
        std::string unit_src = "";
        indentation_level = 0;
        indentation_spaces = 4;

        // TODO: We need to pre-declare all functions first, then generate code
        // Otherwise some function might not be found.

        // Process procedures first:
        for (auto &item : x.m_global_scope->scope) {
            if (item.second->type != ASR::asrType::prog) {
                visit_asr(*item.second);
                unit_src += src;
            }
        }

        // Then the main program:
        for (auto &item : x.m_global_scope->scope) {
            if (item.second->type == ASR::asrType::prog) {
                visit_asr(*item.second);
                unit_src += src;
            }
        }

        src = unit_src;
    }

    void visit_Program(const ASR::Program_t &x) {
        // Generate code for nested subroutines and functions first:
        std::string contains;
        for (auto &item : x.m_symtab->scope) {
            if (item.second->type == ASR::asrType::sub) {
                ASR::Subroutine_t *s = SUBROUTINE(item.second);
                visit_Subroutine(*s);
                contains += src + "\n";
            }
        }

        // Generate code for the main program
        indentation_level += 1;
        std::string indent(indentation_level*indentation_spaces, ' ');
        std::string decl;
        for (auto &item : x.m_symtab->scope) {
            if (item.second->type == ASR::asrType::var) {
                ASR::var_t *v2 = (ASR::var_t*)(item.second);
                ASR::Variable_t *v = (ASR::Variable_t *)v2;
                decl += indent;

                if (v->m_type->type == ASR::ttypeType::Integer) {
                    decl += "int " + std::string(v->m_name) + ";\n";
                } else if (v->m_type->type == ASR::ttypeType::Real) {
                    decl += "float " + std::string(v->m_name) + ";\n";
                } else if (v->m_type->type == ASR::ttypeType::Logical) {
                    decl += "bool " + std::string(v->m_name) + ";\n";
                } else {
                    throw CodeGenError("Variable type not supported");
                }
            }
        }

        std::string body;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            body += src;
        }

        std::string headers =
R"(#include <iostream>
#include <Kokkos_Core.hpp>

)";

        src = headers + contains + "int main(int argc, char* argv[])\n{\n"
                + indent + "Kokkos::initialize(argc, argv); {\n"
                + decl + body
                + indent + "} Kokkos::finalize();\n"
                + indent + "return 0;\n}\n";
        indentation_level -= 1;
    }

    void visit_Subroutine(const ASR::Subroutine_t &x) {
        indentation_level += 1;
        std::string sub = "void " + std::string(x.m_name) + "(";
        for (size_t i=0; i<x.n_args; i++) {
            ASR::Variable_t *arg = VARIABLE((ASR::asr_t*)EXPR_VAR((ASR::asr_t*)x.m_args[i])->m_v);
            LFORTRAN_ASSERT(is_arg_dummy(arg->m_intent));
            sub += convert_variable_decl(*arg);
            if (i < x.n_args-1) sub += ", ";
        }
        sub += ")\n";

        for (auto &item : x.m_symtab->scope) {
            if (item.second->type == ASR::asrType::var) {
                ASR::var_t *v2 = (ASR::var_t*)(item.second);
                ASR::Variable_t *v = (ASR::Variable_t *)v2;
                if (v->m_intent == intent_local) {
                    SymbolInfo s;
                    s.needs_declaration = true;
                    sym_info[get_hash((ASR::asr_t*)v)] = s;
                }
            }
        }

        std::string body;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            body += src;
        }

        std::string decl;
        for (auto &item : x.m_symtab->scope) {
            if (item.second->type == ASR::asrType::var) {
                ASR::var_t *v2 = (ASR::var_t*)(item.second);
                ASR::Variable_t *v = (ASR::Variable_t *)v2;
                if (v->m_intent == intent_local) {
                    if (sym_info[get_hash((ASR::asr_t*) v)].needs_declaration) {
                        std::string indent(indentation_level*indentation_spaces, ' ');
                        decl += indent;
                        decl += convert_variable_decl(*v) + ";\n";
                    }
                }
            }
        }

        sub += "{\n" + decl + body + "}\n";
        src = sub;
        indentation_level -= 1;
    }

    void visit_Function(const ASR::Function_t &x) {
        if (std::string(x.m_name) == "size" && x.n_body == 0) {
            // Intrinsic function `size`
            SymbolInfo s;
            s.intrinsic_function = true;
            sym_info[get_hash((ASR::asr_t*)&x)] = s;
            src = "";
            return;
        } else {
            SymbolInfo s;
            s.intrinsic_function = false;
            sym_info[get_hash((ASR::asr_t*)&x)] = s;
        }
        std::string sub = "int " + std::string(x.m_name) + "(";
        for (size_t i=0; i<x.n_args; i++) {
            ASR::Variable_t *arg = VARIABLE((ASR::asr_t*)EXPR_VAR((ASR::asr_t*)x.m_args[i])->m_v);
            LFORTRAN_ASSERT(is_arg_dummy(arg->m_intent));
            sub += convert_variable_decl(*arg);
            if (i < x.n_args-1) sub += ", ";
        }
        sub += ")\n";

        std::string decl;
        for (auto &item : x.m_symtab->scope) {
            if (item.second->type == ASR::asrType::var) {
                ASR::var_t *v2 = (ASR::var_t*)(item.second);
                ASR::Variable_t *v = (ASR::Variable_t *)v2;
                if (v->m_intent == intent_local) {
                   decl += "    " + convert_variable_decl(*v) + ";\n";
                }
            }
        }

        std::string body;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            body += "    " + src;
        }

        if (decl.size() > 0 || body.size() > 0) {
            sub += "{\n" + decl + body + "}\n";
        } else {
            sub[sub.size()-1] = ';';
            sub += "\n";
        }
        src = sub;
    }

    void visit_FuncCall(const ASR::FuncCall_t &x) {
        ASR::Function_t *fn = FUNCTION((ASR::asr_t*)x.m_func);
        std::string fn_name = fn->m_name;
        if (sym_info[get_hash((ASR::asr_t*)x.m_func)].intrinsic_function) {
            if (fn_name == "size") {
                LFORTRAN_ASSERT(x.n_args > 0);
                visit_expr(*x.m_args[0]);
                std::string var_name = src;
                std::string args;
                if (x.n_args == 1) {
                    args = "0";
                } else {
                    for (size_t i=1; i<x.n_args; i++) {
                        visit_expr(*x.m_args[i]);
                        args += src + "-1";
                        if (i < x.n_args-1) args += ", ";
                    }
                }
                src = var_name + ".extent(" + args + ")";
            } else {
                throw CodeGenError("Intrinsic function '" + fn_name
                        + "' not implemented");
            }

        } else {
            std::string args;
            for (size_t i=0; i<x.n_args; i++) {
                visit_expr(*x.m_args[i]);
                args += src;
                if (i < x.n_args-1) args += ", ";
            }
            src = fn_name + "(" + args + ")";
        }
        last_unary_plus = false;
        last_binary_plus = false;
    }

    void visit_Assignment(const ASR::Assignment_t &x) {
        std::string target;
        if (x.m_target->type == ASR::exprType::Var) {
            ASR::var_t *t1 = EXPR_VAR((ASR::asr_t*)(x.m_target))->m_v;
            target = VARIABLE((ASR::asr_t*)t1)->m_name;
        } else if (x.m_target->type == ASR::exprType::ArrayRef) {
            visit_ArrayRef(*(ASR::ArrayRef_t*)x.m_target);
            target = src;
        } else {
            LFORTRAN_ASSERT(false)
        }
        this->visit_expr(*x.m_value);
        std::string value = src;
        std::string indent(indentation_level*indentation_spaces, ' ');
        src = indent + target + " = " + value + ";\n";
    }

    void visit_Num(const ASR::Num_t &x) {
        src = std::to_string(x.m_n);
        last_unary_plus = false;
        last_binary_plus = false;
    }

    void visit_Str(const ASR::Str_t &x) {
        src = "\"" + std::string(x.m_s) + "\"";
        last_unary_plus = false;
        last_binary_plus = false;
    }

    void visit_Constant(const ASR::Constant_t &x) {
        if (x.m_value == true) {
            src = "true";
        } else {
            src = "false";
        }
        last_unary_plus = false;
        last_binary_plus = false;
    }


    void visit_Var(const ASR::Var_t &x) {
        src = VARIABLE((ASR::asr_t*)(x.m_v))->m_name;
        last_unary_plus = false;
        last_binary_plus = false;
    }

    void visit_ArrayRef(const ASR::ArrayRef_t &x) {
        std::string out = VARIABLE((ASR::asr_t*)(x.m_v))->m_name;
        out += "[";
        for (size_t i=0; i<x.n_args; i++) {
            visit_expr(*x.m_args[i].m_right);
            out += src;
            if (i < x.n_args-1) out += ",";
        }
        out += "]";
        src = out;
        last_unary_plus = false;
        last_binary_plus = false;
    }

    void visit_Compare(const ASR::Compare_t &x) {
        this->visit_expr(*x.m_left);
        std::string left = src;
        this->visit_expr(*x.m_right);
        std::string right = src;
        switch (x.m_op) {
            case (ASR::cmpopType::Eq) : {
                src = left + " == " + right;
                break;
            }
            case (ASR::cmpopType::Gt) : {
                src = left + " > " + right;
                break;
            }
            case (ASR::cmpopType::GtE) : {
                src = left + " >= " + right;
                break;
            }
            case (ASR::cmpopType::Lt) : {
                src = left + " < " + right;
                break;
            }
            case (ASR::cmpopType::LtE) : {
                src = left + " <= " + right;
                break;
            }
            case (ASR::cmpopType::NotEq) : {
                src = left + " != " + right;
                break;
            }
            default : {
                throw CodeGenError("Comparison operator not implemented");
            }
        }
    }

    void visit_UnaryOp(const ASR::UnaryOp_t &x) {
        this->visit_expr(*x.m_operand);
        if (x.m_type->type == ASR::ttypeType::Integer) {
            if (x.m_op == ASR::unaryopType::UAdd) {
                // src = src;
                last_unary_plus = false;
                return;
            } else if (x.m_op == ASR::unaryopType::USub) {
                src = "-" + src;
                last_unary_plus = true;
                last_binary_plus = false;
                return;
            } else {
                throw CodeGenError("Unary type not implemented yet");
            }
        } else if (x.m_type->type == ASR::ttypeType::Logical) {
            if (x.m_op == ASR::unaryopType::Not) {
                src = "!" + src;
                last_unary_plus = false;
                last_binary_plus = false;
                return;
            } else {
                throw CodeGenError("Unary type not implemented yet in Logical");
            }
        } else {
            throw CodeGenError("UnaryOp: type not supported yet");
        }
    }

    void visit_BinOp(const ASR::BinOp_t &x) {
        this->visit_expr(*x.m_left);
        std::string left_val = src;
        if ((last_binary_plus || last_unary_plus)
                    && (x.m_op == ASR::operatorType::Mul
                     || x.m_op == ASR::operatorType::Div)) {
            left_val = "(" + left_val + ")";
        }
        if (last_unary_plus
                    && (x.m_op == ASR::operatorType::Add
                     || x.m_op == ASR::operatorType::Sub)) {
            left_val = "(" + left_val + ")";
        }
        this->visit_expr(*x.m_right);
        std::string right_val = src;
        if ((last_binary_plus || last_unary_plus)
                    && (x.m_op == ASR::operatorType::Mul
                     || x.m_op == ASR::operatorType::Div)) {
            right_val = "(" + right_val + ")";
        }
        if (last_unary_plus
                    && (x.m_op == ASR::operatorType::Add
                     || x.m_op == ASR::operatorType::Sub)) {
            right_val = "(" + right_val + ")";
        }
        switch (x.m_op) {
            case ASR::operatorType::Add: {
                src = left_val + " + " + right_val;
                last_binary_plus = true;
                break;
            }
            case ASR::operatorType::Sub: {
                src = left_val + " - " + right_val;
                last_binary_plus = true;
                break;
            }
            case ASR::operatorType::Mul: {
                src = left_val + "*" + right_val;
                last_binary_plus = false;
                break;
            }
            case ASR::operatorType::Div: {
                src = left_val + "/" + right_val;
                last_binary_plus = false;
                break;
            }
            case ASR::operatorType::Pow: {
                src = "std::pow(" + left_val + ", " + right_val + ")";
                last_binary_plus = false;
                break;
            }
            default : throw CodeGenError("Unhandled switch case");
        }
        last_unary_plus = false;
    }


    void visit_Print(const ASR::Print_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        std::string out = indent + "std::cout ";
        for (size_t i=0; i<x.n_values; i++) {
            this->visit_expr(*x.m_values[i]);
            out += "<< " + src + " ";
        }
        out += "<< std::endl;\n";
        src = out;
    }

    void visit_WhileLoop(const ASR::WhileLoop_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        std::string out = indent + "while (";
        visit_expr(*x.m_test);
        out += src + ") {\n";
        indentation_level += 1;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            out += src;
        }
        out += indent + "};\n";
        indentation_level -= 1;
        src = out;
    }

    void visit_Exit(const ASR::Exit_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        src = indent + "break;\n";
    }

    void visit_Cycle(const ASR::Cycle_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        src = indent + "continue;\n";
    }

    void visit_DoLoop(const ASR::DoLoop_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        std::string out = indent + "for (";
        ASR::Variable_t *loop_var = VARIABLE((ASR::asr_t*)EXPR_VAR((ASR::asr_t*)x.m_head.m_v)->m_v);
        std::string lvname=loop_var->m_name;
        ASR::expr_t *a=x.m_head.m_start;
        ASR::expr_t *b=x.m_head.m_end;
        ASR::expr_t *c=x.m_head.m_increment;
        LFORTRAN_ASSERT(a);
        LFORTRAN_ASSERT(b);
        int increment;
        if (!c) {
            increment = 1;
        } else {
            if (c->type == ASR::exprType::Num) {
                increment = EXPR_NUM((ASR::asr_t*)c)->m_n;
            } else if (c->type == ASR::exprType::UnaryOp) {
                ASR::UnaryOp_t *u = EXPR_UNARYOP((ASR::asr_t*)c);
                LFORTRAN_ASSERT(u->m_op == ASR::unaryopType::USub);
                LFORTRAN_ASSERT(u->m_operand->type == ASR::exprType::Num);
                increment = - EXPR_NUM((ASR::asr_t*)u->m_operand)->m_n;
            } else {
                throw CodeGenError("Do loop increment type not supported");
            }
        }
        std::string cmp_op;
        if (increment > 0) {
            cmp_op = "<=";
        } else {
            cmp_op = ">=";
        }

        out += lvname + "=";
        visit_expr(*a);
        out += src + "; " + lvname + cmp_op;
        visit_expr(*b);
        out += src + "; " + lvname;
        if (increment == 1) {
            out += "++";
        } else if (increment == -1) {
            out += "--";
        } else {
            out += "+=" + std::to_string(increment);
        }
        out += ") {\n";
        indentation_level += 1;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            out += src;
        }
        out += indent + "};\n";
        indentation_level -= 1;
        src = out;
    }

    void visit_DoConcurrentLoop(const ASR::DoConcurrentLoop_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        std::string out = indent + "Kokkos::parallel_for(";
        visit_expr(*x.m_head.m_end);
        out += src;
        ASR::Variable_t *loop_var = VARIABLE((ASR::asr_t*)EXPR_VAR((ASR::asr_t*)x.m_head.m_v)->m_v);
        sym_info[get_hash((ASR::asr_t*) loop_var)].needs_declaration = false;
        out += ", KOKKOS_LAMBDA(const long " + std::string(loop_var->m_name)
                + ") {\n";
        indentation_level += 1;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            out += src;
        }
        out += indent + "});\n";
        indentation_level -= 1;
        src = out;
    }

    void visit_ErrorStop(const ASR::ErrorStop_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        src = indent + "std::cerr << \"ERROR STOP\" << std::endl;\n";
        src += indent + "exit(1);\n";
    }

    void visit_If(const ASR::If_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        std::string out = indent + "if (";
        visit_expr(*x.m_test);
        out += src + ") {\n";
        indentation_level += 1;
        for (size_t i=0; i<x.n_body; i++) {
            this->visit_stmt(*x.m_body[i]);
            out += src;
        }
        out += indent + "}";
        if (x.n_orelse == 0) {
            out += ";\n";
        } else {
            out += " else {\n";
            for (size_t i=0; i<x.n_orelse; i++) {
                this->visit_stmt(*x.m_orelse[i]);
                out += src;
            }
            out += indent + "};\n";
        }
        indentation_level -= 1;
        src = out;
    }

    void visit_SubroutineCall(const ASR::SubroutineCall_t &x) {
        std::string indent(indentation_level*indentation_spaces, ' ');
        ASR::Subroutine_t *s = SUBROUTINE((ASR::asr_t*)x.m_name);
        std::string out = indent + s->m_name + "(";
        for (size_t i=0; i<x.n_args; i++) {
            if (x.m_args[i]->type == ASR::exprType::Var) {
                ASR::Variable_t *arg = VARIABLE((ASR::asr_t*)EXPR_VAR((ASR::asr_t*)x.m_args[i])->m_v);
                std::string arg_name = arg->m_name;
                out += arg_name;
            } else {
                this->visit_expr(*x.m_args[i]);
                out += src;
            }
            if (i < x.n_args-1) out += ", ";
        }
        out += ");\n";
        src = out;
    }

};

std::string asr_to_cpp(ASR::asr_t &asr)
{
    ASRToCPPVisitor v;
    LFORTRAN_ASSERT(asr.type == ASR::asrType::unit);
    v.visit_asr(asr);
    return v.src;
}

} // namespace LFortran
