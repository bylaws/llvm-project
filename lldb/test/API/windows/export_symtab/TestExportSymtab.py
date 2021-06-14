"""
Test that export table does not overwrite COFF symbols (on Windows).
"""

import lldb
import lldbsuite.test.lldbutil as lldbutil
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *


class TestExportSymtab(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    # If your test case doesn't stress debug info, the
    # set this to true.  That way it won't be run once for
    # each debug info format.
    NO_DEBUG_INFO_TESTCASE = True

    @skipIf(oslist=no_match(["windows"]))
    @skipIf(archs=no_match(["x86_64"]))
    def test_export_symtab(self):
        self.build()
        exe = self.getBuildArtifact("a.out")

        target = self.dbg.CreateTarget(exe)
        error = lldb.SBError()
        launch_info = lldb.SBLaunchInfo(None)
        process = target.Launch(launch_info, error)

        thread = process.GetSelectedThread()
        # Verify that the stack trace contains correct names of the symbols.
        self.assertEqual("int f(void)", thread.frames[0].GetFunctionName())
        self.assertEqual("int g(void)", thread.frames[1].GetFunctionName())
        self.assertEqual("main", thread.frames[2].GetFunctionName())