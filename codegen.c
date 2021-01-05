#include "occ.h"

static int depth;
// x86_64 calling convention
// (https://en.wikipedia.org/wiki/X86_calling_conventions#x86-64_calling_conventions)
static char *argreg8[] = {"dil", "sil", "dl", "cl", "r8b", "r9b"};
static char *argreg64[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
static Obj *current_fn;

static void gen_stmt(Node *node);
static void gen_expr(Node *node);

static int count(void) {
  static int i = 1;
  return i++;
}

static void push(void) {
  printf("  push rax\n");
  depth++;
}

static void pop(char *arg) {
  printf("  pop %s\n", arg);
  depth--;
}

// Round up `n` to the nearest multiple of `align`, For example,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

// Compute the absolute address of a given node.
static void gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->is_local)
      printf("  lea rax, [rbp-%d]\n", -node->var->offset);
    else
      printf("  lea rax, %s[rip]\n", node->var->name);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_addr(node->rhs);
    return;
  case ND_MEMBER:
    gen_addr(node->lhs);
    printf("  add rax, %d\n", node->member->offset);
    return;
  }

  error_tok(node->tok, "not a lvalue");
}

// Load a value from where RAX is pointing to.
static void load(Type *ty) {
  if (ty->kind == TY_ARRAY)
    return;

  if (ty->size == 1)
    printf("  movsbq rax, [rax]\n");
  else
    printf("  mov rax, [rax]\n");
}

// Store RAX to an address that the stack top is pointing to.
static void store(Type *ty) {
  pop("rdi");

  if (ty->size == 1)
    printf("  mov [rdi], al\n");
  else
    printf("  mov [rdi], rax\n");
}

static void gen_expr(Node *node) {
  printf("  .loc 1 %d\n", node->tok->line_no);

  switch (node->kind) {
  case ND_NUM:
    printf("  mov rax, %d\n", node->val);
    return;
  case ND_NEG:
    gen_expr(node->lhs);
    printf("  neg rax\n");
    return;
  case ND_VAR:
  case ND_MEMBER:
    gen_addr(node);
    load(node->ty);
    return;
  case ND_DEREF:
    gen_expr(node->lhs);
    load(node->ty);
    return;
  case ND_ADDR:
    gen_addr(node->lhs);
    return;
  case ND_ASSIGN:
    gen_addr(node->lhs);
    push();
    gen_expr(node->rhs);
    store(node->ty);
    return;
  case ND_COMMA:
    gen_expr(node->lhs);
    gen_expr(node->rhs);
    return;
  case ND_FUNCALL: {
    int nargs = 0;
    for (Node *arg = node->args; arg; arg = arg->next) {
      gen_expr(arg);
      push();
      nargs++;
    }

    for (int i = nargs - 1; i >= 0; i--)
      pop(argreg64[i]);

    printf("  mov rax, 0\n");
    printf("  call %s\n", node->funcname);
    return;
  }
  case ND_STMT_EXPR:
    for (Node *stmt = node->body; stmt; stmt = stmt->next)
      gen_stmt(stmt);
    return;
  }

  gen_expr(node->rhs);
  push();              // rhs on rax
  gen_expr(node->lhs);
  pop("rdi");          // lhs on rdi

  switch (node->kind) {
  case ND_ADD:
    printf("  add rax, rdi\n");
    return;
  case ND_SUB:
    printf("  sub rax, rdi\n");
    return;
  case ND_MUL:
    printf("  imul rax, rdi\n");
    return;
  case ND_DIV:
    printf("  cqo\n");
    printf("  idiv rdi\n");
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    printf("  cmp rax, rdi\n");

    if (node->kind == ND_EQ)
      printf("  sete al\n");
    else if (node->kind == ND_NE)
      printf("  setne al\n");
    else if (node->kind == ND_LT)
      printf("  setl al\n");
    else if (node->kind == ND_LE)
      printf("  setle al\n");

    printf("  movzb rax, al\n");
    return;
  }

  error_tok(node->tok, "invalid expression");
}

static void gen_stmt(Node *node) {
  printf("  .loc 1 %d\n", node->tok->line_no);

  switch (node->kind) {
  case ND_IF: {
    int c = count();
    gen_expr(node->cond);
    printf("  cmp rax, 0\n");
    printf("  je .L.else.%d\n", c);
    gen_stmt(node->then);
    printf("  jmp .L.end.%d\n", c);
    printf(".L.else.%d:\n", c);
    if (node->els)
      gen_stmt(node->els);
    printf(".L.end.%d:\n", c);
    return;
  }
  case ND_FOR: {
    int c = count();
    if (node->init)
      gen_stmt(node->init);
    printf(".L.begin.%d:\n", c);
    if (node->cond) {
      gen_expr(node->cond);
      printf("  cmp rax, 0\n");
      printf("  je .L.end.%d\n", c);
    }
    gen_stmt(node->then);
    if (node->inc)
      gen_expr(node->inc);
    printf("  jmp .L.begin.%d\n", c);
    printf(".L.end.%d:\n", c);
    return;
  }
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  case ND_RETURN:
    gen_expr(node->lhs);
    printf("  jmp .L.return.%s\n", current_fn->name);
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  }

  error_tok(node->tok, "invalid statement");
}

// Assign offsets to local varialbes.
static void assign_lvar_offsets(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function)
      continue;

    int offset = 0;
    for (Obj *var = fn->locals; var; var = var->next) {
      // FIXME: Varialbe order is reverse!!
      offset += var->ty->size;
      var->offset = -offset;
    }
    fn->stack_size = align_to(offset, 16);
  }
}

static void emit_data(Obj *prog) {
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function)
      continue;

    printf("  .data\n");
    printf("  .globl %s\n", var->name);
    printf("%s:\n", var->name);

    if (var->init_data) {
      for (int i = 0; i < var->ty->size; i++)
        printf("  .byte %d\n", var->init_data[i]);
    } else {
      printf("  .zero %d\n", var->ty->size);
    }
  }
}

static void emit_text(Obj *prog) {
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function)
      continue;

    printf("  .globl %s\n", fn->name);
    printf("  .text\n");
    printf("%s:\n", fn->name);
    current_fn = fn;

    // Prologue
    printf("  push rbp\n");
    printf("  mov rbp, rsp\n");
    printf("  sub rsp, %d\n", fn->stack_size);

    // Save passed-by-register arguments to the stack.
    int i = 0;
    for (Obj *var = fn->params; var; var = var->next) {
      if (var->ty->size == 1)
        printf("  mov [rbp-%d], %s\n", -var->offset, argreg8[i++]);
      else
        printf("  mov [rbp-%d], %s\n", -var->offset, argreg64[i++]);
    }

    // Emit code
    gen_stmt(fn->body);
    assert(depth == 0);

    // Epilogue
    printf(".L.return.%s:\n", fn->name);
    printf("  mov rsp, rbp\n");
    printf("  pop rbp\n");
    printf("  ret\n");
  }
}

void codegen(Obj *prog) {
  assign_lvar_offsets(prog);
  printf("  .intel_syntax noprefix\n");
  emit_data(prog);
  emit_text(prog);
}
