/* Functions with parameters, nested calls, and recursion. */
int add(int a, int b) {
    return a + b;
}

int mul(int a, int b) {
    return a * b;
}

int fact(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * fact(n - 1);
}

int main() {
    print(add(3, 4));
    print(mul(6, 7));
    print(fact(5));
    print(add(mul(2, 3), 4));
    return add(1, 2);
}
