#!/usr/bin/env python3
"""Offline regression tests for pre-build diagnostic aggregation."""
from datetime import datetime, timezone
import importlib.util
from pathlib import Path
import unittest

script=Path(__file__).resolve().parents[1]/".github/scripts/analyze-user-telemetry.py"
spec=importlib.util.spec_from_file_location("iso_wcx_diagnostics",script)
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def issue(*, count=3, confirmed=False, reason="verification_failed", code=13, title=True):
    return {
        "title":("ISO-WCX Diagnostics: 0.21.1 RC1" if title else "Unrelated issue"),
        "created_at":datetime.now(timezone.utc).isoformat(),
        "body":("# ISO-WCX Diagnostic Summary\n\n"
                "| Build | Format | Action | WCX code | Category | Count |\n"
                "|---|---|---|---:|---|---:|\n"
                f"| 0.21.1 RC1 B25 | iso | pack | {code} | {reason} | {count} |\n"),
        "labels":([{"name":module.REGRESSION_LABEL}] if confirmed else []),
    }

class RegressionTests(unittest.TestCase):
    def test_no_report_is_not_a_clean_bill_of_health(self):
        result=module.analyze([],available=False)
        self.assertFalse(result["source_available"])
        self.assertIn("unavailable",module.markdown(result))
    def test_unconfirmed_user_failure_never_blocks(self):
        result=module.analyze([issue(count=20)])
        self.assertEqual(result["operation_results"],20)
        self.assertEqual(result["critical_confirmed"],[])
    def test_confirmed_repeat_is_a_build_gate(self):
        result=module.analyze([issue(confirmed=True)])
        self.assertEqual(len(result["critical_confirmed"]),1)
    def test_malicious_or_unrelated_text_is_ignored(self):
        result=module.analyze([
            issue(title=False,count=99,confirmed=True),
            issue(count=50,confirmed=True,reason="../../PRIVATE"),
            issue(count=7,confirmed=True,reason="unsupported_layout",code=24),
        ])
        self.assertEqual(result["critical_confirmed"],[])
        doc=module.markdown(result)
        self.assertNotIn("../../",doc)
    def test_prs_are_not_considered_issues(self):
        row=issue(confirmed=True)
        row["pull_request"]={"url":"https://github.com/example"}
        self.assertEqual(module.analyze([row])["issues"],0)

if __name__=="__main__":
    unittest.main()
