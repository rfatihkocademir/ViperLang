#include <stdio.h>

typedef struct {
    int id;
    double value;
} TestStruct;

TestStruct mutate_struct(TestStruct in) {
    printf("C Native: Received struct { id: %d, value: %f }\n", in.id, in.value);
    TestStruct out;
    out.id = in.id + 10;
    out.value = in.value * 2.5;
    return out;
}
