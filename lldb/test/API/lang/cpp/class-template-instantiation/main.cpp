template <typename T> struct foo { static int x; };

template <typename T> int foo<T>::x = 42 + sizeof(T);

template <typename T>
struct S {
  T t;
};

struct A {
  template <typename T> struct bar { T f; };

  bar<int> bi;
  bar<short> bs;
  S<S<S<int>>> si;
  S<S<S<double>>> sd;

  int size() {
    return sizeof(bar<int>) + sizeof(bar<short>); // break method
  }
};

int main() {
  A a;
  a.size();
  return foo<char>::x + foo<int>::x; // break main
}
