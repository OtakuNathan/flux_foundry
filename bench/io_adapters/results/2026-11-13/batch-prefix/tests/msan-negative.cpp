int main() {
    volatile int* value = new int;
    const int result = *value;
    delete value;
    return result;
}
