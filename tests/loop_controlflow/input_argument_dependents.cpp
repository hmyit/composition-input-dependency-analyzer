#include <iostream>

int sum(int n)
{
    int sum = 0;
    for (unsigned i = 0; i < n; ++i) {
        sum += i;
    }
    return sum;
}

int evens_sum(int n)
{
    int sum = 0;
    unsigned i = 0;
    for (; i < n; ) {
        if (i % 2 == 0) {
            sum += i;
        }
        ++i;
    }
    return sum;
}

int mul(int n)
{
    int mul = 1;
    int i = n;
    while (true) {
        if (i == 0) {
            break;
        }
        if (i == 1) {
            continue;
        }
        mul *= i;
        --i;
    }
    return mul;
}

int complex_sum(int n)
{
    unsigned i = 0;
    int sum = 0;
    while (i < n) {
        for (unsigned j = i; ; ++j) {
            if (j == n) {
                break;
            }
            sum += j;
        }
        ++i;
    }
    return sum;
}


int main(int argc, char* argv[])
{
    const unsigned n = argc;
    unsigned s = 0;
    s = sum(n);
    unsigned evens = evens_sum(argc * 2);
    unsigned m = mul(evens);
    s = complex_sum(m);

    return 0;
}

