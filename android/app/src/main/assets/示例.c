/* 示例.c - 一个用于体验代码编辑功能的小例子（FizzBuzz 变体）。 */
#include <stdio.h>

#define LIMIT 30

/* 返回带颜色标记的 FizzBuzz 结果 */
static const char *classify(int n) {
    if (n % 15 == 0) return "FizzBuzz";
    if (n % 3  == 0) return "Fizz";
    if (n % 5  == 0) return "Buzz";
    return NULL;
}

int main(int argc, char **argv) {
    for (int i = 1; i <= LIMIT; i++) {
        const char *label = classify(i);
        if (label)
            printf("%2d: %s\n", i, label);
        else
            printf("%2d: %d\n", i, i);
    }
    return 0;
}
