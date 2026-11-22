/* void functions, globals shared between functions, and recursion
   (fibonacci) with calls inside expressions. */
int counter;

void greet(int n) {
    print(n);
    counter = counter + 1;
}

int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main() {
    greet(7);
    greet(8);
    print(counter);
    print(fib(10));

    int i = 0;
    while (i < 3) {
        greet(100 + i);
        i++;
    }
    print(counter);
    return fib(6);
}
