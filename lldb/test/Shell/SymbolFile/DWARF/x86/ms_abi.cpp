// REQUIRES: lld

// RUN: %clang -c --target=x86_64-windows-msvc -gdwarf %s -o %t.obj
// RUN: lld-link /out:%t.exe %t.obj /nodefaultlib /entry:main /debug
// RUN: %lldb -f %t.exe \
// RUN:   -o "target variable mp1 mp2 mp3 mp4 mp5 mp6 mp7 mp8 mp9" -o "exit" \
// RUN:   | FileCheck %s

// The following tests that MSInheritanceAttr are set for record decls.
class SI {
  int si;
};
struct SI2 {
  int si2;
};
class MI : SI, SI2 {
  int mi;
};
class MI2 : MI {
  int mi2;
};
class VI : virtual MI {
  int vi;
};
class VI2 : virtual SI, virtual SI2 {
  int vi;
};
class /* __unspecified_inheritance*/ UI;

typedef void (SI::*SITYPE)();
typedef void (MI::*MITYPE)();
typedef void (MI2::*MI2TYPE)();
typedef void (VI::*VITYPE)();
typedef void (VI2::*VI2TYPE)();
typedef void (UI::*UITYPE)();
SITYPE mp1 = nullptr;
MITYPE mp2 = nullptr;
MI2TYPE mp3 = nullptr;
VITYPE mp4 = nullptr;
VI2TYPE mp5 = nullptr;
UITYPE mp6 = nullptr;
MITYPE *mp7 = nullptr;
VI2TYPE *mp8 = nullptr;
int SI::*mp9 = nullptr;

// CHECK: (SITYPE) mp1 = 00 00 00 00 00 00 00 00
// CHECK: (MITYPE) mp2 = 00 00 00 00 00 00 00 00
// CHECK: (MI2TYPE) mp3 = 00 00 00 00 00 00 00 00
// CHECK: (VITYPE) mp4 = 00 00 00 00 00 00 00 00
// CHECK: (VI2TYPE) mp5 = 00 00 00 00 00 00 00 00
// CHECK: (UITYPE) mp6 = 00 00 00 00 00 00 00 00
// CHECK: (MITYPE *) mp7 = 0x0000000000000000
// CHECK: (VI2TYPE *) mp8 = 0x0000000000000000
// CHECK: (int SI::*) mp9 = ff ff ff ff

int main() {}
