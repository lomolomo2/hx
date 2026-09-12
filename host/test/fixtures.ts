// The parts of the test fixtures that **must** vary by platform.
//
// ★ Why not settle on one language: the interpreters that actually run inside
//   the sandbox differ between the platforms. Each side picks the one that is
//   installed in a system directory and therefore naturally visible inside the
//   sandbox -- this is not a compromise, it is the same principle producing
//   different answers on two platforms.
//
//   Linux   uses python3 -- it lives in /usr/bin, one of the default read-only
//           paths. node cannot be used: nvm installs node under $HOME, and
//           $HOME is unreadable by default (the first of the README's "known
//           traps"), so the tests would pass or fail depending on the machine.
//
//   Windows uses powershell.exe -- it lives under %SystemRoot%\System32 and
//           ships granted to ALL APPLICATION PACKAGES.
//
//   The candidates ruled out on Windows, with the measured results recorded
//   here so nobody has to try them again:
//     - python3 -- a Microsoft Store placeholder; running it only prints a
//       prompt to install from the Store. The real python.exe installs into
//       %LOCALAPPDATA%, which the sandbox cannot read
//     - node    -- installed in Program Files, but the Node installer
//       **breaks ACL inheritance**, so that directory has no ALL APPLICATION
//       PACKAGES ACE and an AppContainer cannot start it
//     - git / cscript -- likewise cannot start
//
//   This is the Windows version of "the toolchain in $HOME will not run", and
//   slightly worse: even things in Program Files can be affected, and fixing
//   the ACL requires administrator rights.
//   The test is whether the target directory grants ALL APPLICATION PACKAGES,
//   and `icacls <dir>` shows it at a glance.
import { isWindows } from "../src/platform.js";

export interface Fixture {
  /** The source file being fixed */
  sourceName: string;
  sourceBroken: string;
  /** The file that runs the test */
  testName: string;
  testBody: string;
  /** The command that runs the test (handed to the bash tool and wrapped by
   *  platform.shellCommand) */
  runCommand: string;
  /** The patch that turns source from broken into fixed */
  patch: string;
  /** What the source file should contain once fixed */
  fixedMarker: string;
}

const python: Fixture = {
  sourceName: "calc.py",
  sourceBroken: "def add(a, b):\n    return a - b\n",
  testName: "test_calc.py",
  testBody: 'from calc import add\nassert add(2, 3) == 5\nprint("ALL TESTS PASS")\n',
  // -B: write no .pyc. The patch changes "a - b" to "a + b" without changing
  // the byte count, and if it also lands within the same second, Python's
  // (mtime seconds, size) check judges the cache still valid -- so the fix is
  // correct yet the test still reports failure.
  runCommand: "python3 -B test_calc.py",
  patch: `*** Begin Patch
*** Update File: calc.py
@@
 def add(a, b):
-    return a - b
+    return a + b
*** End Patch
`,
  fixedMarker: "return a + b",
};

const powershell: Fixture = {
  sourceName: "calc.ps1",
  sourceBroken: "function Add-Values($a, $b) {\r\n    return $a - $b\r\n}\r\n",
  testName: "test_calc.ps1",
  // All three of these were learned the hard way:
  //   - $ErrorActionPreference = Stop -- otherwise Write-Error does not halt
  //     execution, the script exits 0 with a screen full of errors, and the
  //     "test passes" while verifying nothing
  //   - $PSScriptRoot -- dot-sourcing ".\calc.ps1" gives CommandNotFound:
  //     PowerShell's provider location is not necessarily the process cwd
  //   - -ExecutionPolicy Bypass on the command line -- the default policy
  //     refuses to run .ps1 files, and that is machine configuration; an
  //     end-to-end test's outcome must not depend on it
  testBody:
    "$ErrorActionPreference = 'Stop'\r\n" +
    '. "$PSScriptRoot\\calc.ps1"\r\n' +
    "$r = Add-Values 2 3\r\n" +
    "if ($r -ne 5) { Write-Output \"FAIL: got $r\"; exit 1 }\r\n" +
    "Write-Output 'ALL TESTS PASS'\r\n",
  runCommand: "powershell -NoProfile -ExecutionPolicy Bypass -File test_calc.ps1",
  // ★ The patch itself is LF while the fixture file is CRLF -- deliberately:
  //   it also exercises apply_patch's line-ending handling (see LineEnding in
  //   fs/apply_patch.cpp). Without CRLF handling, this patch reports "context
  //   not found".
  patch: `*** Begin Patch
*** Update File: calc.ps1
@@
 function Add-Values($a, $b) {
-    return $a - $b
+    return $a + $b
 }
*** End Patch
`,
  fixedMarker: "return $a + $b",
};

export const fixture: Fixture = isWindows ? powershell : python;

/** A command that produces a lot of output, used to push the context up to the
 *  compaction threshold. */
export function fillerCommand(step: number): string {
  return isWindows
    ? `powershell -NoProfile -Command "Write-Output ('STEP${step} filler line. ' * 300)"`
    : `python3 -c "print('STEP${step} filler line. ' * 300)"`;
}
