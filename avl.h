#pragma once

typedef struct AVL { struct AVL *l, *r; int h; double score; char *name; } AVL;

AVL *avl_insert(AVL *root, double score, const char *name);
AVL *avl_remove(AVL *root, double score, const char *name);
void avl_range(AVL *root, double min, int *limit, void (*cb)(AVL *, void *), void *ud);
void avl_free(AVL *root);
