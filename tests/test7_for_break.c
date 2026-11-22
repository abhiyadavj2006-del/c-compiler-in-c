/* for loops, break / continue, and else-if chains. */
int main() {
    int total = 0;
    for (int i = 1; i <= 10; i++) {
        total += i;
    }
    print(total);

    int count = 0;
    for (int i = 1; i <= 100; i++) {
        if (i % 2 == 0) {
            continue;
        }
        count++;
        if (count == 3) {
            break;
        }
    }
    print(count);

    int x = 75;
    if (x >= 90) {
        print(1);
    } else if (x >= 60) {
        print(2);
    } else {
        print(3);
    }
    return 0;
}
