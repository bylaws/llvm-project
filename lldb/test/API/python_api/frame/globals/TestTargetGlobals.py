"""
Test SBTarget::FindGlobalVariables API.
"""

from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *

class TargetAPITestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @add_test_categories(['pyapi'])
    def test_find_global_variables(self):
        """Exercise SBTarget.FindGlobalVariables() API."""
        self.build()

        # Don't need to launch a process, since we're only interested in
        # looking up global variables.
        target = self.dbg.CreateTarget(self.getBuildArtifact())

        def test_global_var(query, name, type_name, value):
            value_list = target.FindGlobalVariables(query, 1)
            self.assertEqual(value_list.GetSize(), 1)
            var = value_list.GetValueAtIndex(0)
            self.DebugSBValue(var)
            self.assertTrue(var)
            self.assertEqual(var.GetName(), name)
            self.assertEqual(var.GetTypeName(), type_name)
            self.assertEqual(var.GetValue(), value)

        test_global_var(
            "Vars::inline_static",
            "Vars::inline_static", "double", "1.5")
        test_global_var(
            "Vars::static_constexpr",
            "Vars::static_constexpr", "const int", "2")
        test_global_var(
            "Vars::static_const_out_out_class",
            "Vars::static_const_out_out_class", "const int", "3")
        test_global_var(
            "global_var_of_char_type",
            "::global_var_of_char_type", "char", "'X'")

        test_global_var("eFirst", "::eFirst", "MyEnum", "eFirst")
        test_global_var("A::eMany", "A::eMany", "A::AEnum", "eMany")

        # Global variable eFoo is looked up fine, since scoped enumeration
        # members are not available as constants in the surrounding scope.
        test_global_var("eFoo", "::eFoo", "int", "2")

        # eBar is not available since it's a member of a scoped enumeration.
        value_list = target.FindGlobalVariables("eBar", 1)
        self.assertEqual(value_list.GetSize(), 0)

        # Get enumerator values from all scopes.
        value_list = target.FindGlobalVariables("eMany", 100500)
        self.assertEqual(value_list.GetSize(), 3)
        value_types = {value.GetTypeName() for value in value_list}
        self.assertEqual(value_types, {"A::AEnum", "B::BEnum", "CEnum"})
