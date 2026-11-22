/* char type + character literals, ++/--, compound assignment,
   bitwise operators, and global variables. */
int g = 10;
int h;

int bump(int x) {
    return x + 1;
}

int main() {
    char c = 'A';
    print(c);
    print('a' - 'A');
    print('\n');

    int x = 5;
    x++;
    print(x);
    ++x;
    print(x);
    x--;
    print(x);
    x *= 3;
    print(x);
    x -= 4;
    print(x);

    print(12 & 10);
    print(12 | 10);
    print(12 ^ 10);
    print(1 << 4);
    print(256 >> 4);
    print(~0);

    g = g + 5;
    print(g);
    print(h);
    h = 41;
    print(bump(h));
    return 0;
}
