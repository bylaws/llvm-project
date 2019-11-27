"""
Test that the expression evaluator can instantiate templates. We should be able
to instantiate at least those templates that are instantiated in the symbol
file.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestClassTemplateInstantiation(TestBase):
    mydir = TestBase.compute_mydir(__file__)

    @skipIf(debug_info=no_match(["dwarf"]),oslist=no_match(["macosx"]))
    def test_instantiate_template_from_function(self):
        self.runCmd("settings set target.experimental.infer-class-templates true")
        self.main_source_file = lldb.SBFileSpec("main.cpp")
        self.build()
        (_, _, thread, _) = lldbutil.run_to_source_breakpoint(self, "// break main", self.main_source_file)
        frame = thread.GetSelectedFrame()

        expr_result = frame.EvaluateExpression("foo<char>::x")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "43")

        expr_result = frame.EvaluateExpression("foo<int>::x")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "46")

        expr_result = frame.EvaluateExpression("sizeof(A::bar<short>)")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "2")

        expr_result = frame.EvaluateExpression("sizeof(A::bar<int>)")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "4")

        expr_result = frame.EvaluateExpression("sizeof(S<S<S<int>>>)")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "4")

        expr_result = frame.EvaluateExpression("sizeof(S<S<S<double>>>)")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "8")

    @skipIf(debug_info=no_match(["dwarf"]),oslist=no_match(["macosx"]))
    def test_instantiate_template_from_method(self):
        self.runCmd("settings set target.experimental.infer-class-templates true")
        self.main_source_file = lldb.SBFileSpec("main.cpp")
        self.build()
        (_, _, thread, _) = lldbutil.run_to_source_breakpoint(self, "// break method", self.main_source_file)
        frame = thread.GetSelectedFrame()

        expr_result = frame.EvaluateExpression("sizeof(bar<short>)")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "2")

        expr_result = frame.EvaluateExpression("sizeof(bar<int>)")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "4")

        expr_result = frame.EvaluateExpression("::foo<char>::x")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "43")

        expr_result = frame.EvaluateExpression("foo<int>::x")
        self.assertTrue(expr_result.IsValid())
        self.assertEqual(expr_result.GetValue(), "46")
