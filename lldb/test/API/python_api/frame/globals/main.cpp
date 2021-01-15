class Vars {
public:
  inline static double inline_static = 1.5;
  static constexpr int static_constexpr = 2;
  static const int static_const_out_out_class;
};

const int Vars::static_const_out_out_class = 3;

char global_var_of_char_type = 'X';

namespace A {
enum AEnum { eMany = 0 } ae;
};

struct B {
  enum BEnum { eMany = 1 } be;
} b;

enum CEnum { eMany = 2 } ce;

enum MyEnum {
  eFirst,
} my_enum;

enum class MyScopedEnum {
  eFoo = 1,
  eBar,
} my_scoped_enum;

int eFoo = 2;

int main() {}
