/* Show all brotli distance codes that have extra bits. */

#include <stdio.h>

void codes(int p)
{
    int d, x, b;

    printf("NPOSTFIX = %d\n", p);
    d = 0;
    for (;;) {
        x = 1 + (d >> (p + 1));
        if (x > 24)
            break;
        b = ((((2 + ((d >> p) & 1)) << x) - 4) << p) + (d & ((1 << p) - 1)) + 1;
        if (x == 1)
            printf("%3d: %d, %d\n",
                   d, b, b + (1 << p));
        else
            printf("%3d: %d, %d, ..., %d\n",
                   d, b, b + (1 << p), b + (((1 << x) - 1) << p));
        d++;
    }
    printf("%d codes (expect %d)\n\n", d, 48 << p);
}

int main(void)
{
    codes(0);
    codes(1);
    codes(2);
    codes(3);
    return 0;
}
