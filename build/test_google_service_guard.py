import importlib.util
import pathlib
import unittest


spec = importlib.util.spec_from_file_location(
    "guard", pathlib.Path(__file__).with_name("generate-google-service-guard.py")
)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def annotation(name, kind="Definition", extra="", google=False):
    return {"id": name, "kind": kind, "extra_id": extra,
            "text": "semantics { destination: " +
                    ("GOOGLE_OWNED_SERVICE" if google else "WEBSITE") + " }"}


class GuardTests(unittest.TestCase):
    def test_complete(self):
        self.assertEqual(guard.classify([annotation("metrics", google=True)]), {"metrics"})

    def test_web_content_is_not_blocked(self):
        self.assertEqual(guard.classify([annotation("website")]), set())

    def test_google_partial(self):
        self.assertEqual(guard.classify([annotation("geo", "Partial", "net", True)]), {"geo"})

    def test_completing_uses_partial_id(self):
        self.assertEqual(guard.classify([
            annotation("caller", "Partial", "complete"),
            annotation("complete", "Completing", google=True),
        ]), {"caller"})

    def test_branched_completing_uses_own_id(self):
        self.assertEqual(guard.classify([
            annotation("caller", "Partial", "group"),
            annotation("branch", "BranchedCompleting", "group", True),
        ]), {"branch"})

    def test_branched_inherits_google_destination(self):
        self.assertEqual(guard.classify([
            annotation("caller", "Partial", "group", True),
            annotation("branch", "BranchedCompleting", "group"),
        ]), {"caller", "branch"})

    def test_unrelated_completing_does_not_block(self):
        self.assertEqual(guard.classify([
            annotation("caller", "Partial", "other"),
            annotation("complete", "Completing", google=True),
        ]), set())

    def test_annotation_hash(self):
        self.assertEqual(guard.annotation_hash("abc"), 96354)
        self.assertEqual(guard.annotation_hash("metrics_report_uma"), 727528)

    def test_shared_updater_is_endpoint_scoped(self):
        self.assertIn("update_client", guard.MIXED_DESTINATIONS)
        self.assertNotIn("gaia_auth_list_accounts", guard.MIXED_DESTINATIONS)

    def test_render_keeps_websites_outside_endpoint_gate(self):
        result = guard.render_header({"metrics_report_uma"}, ["google.co.uk"])
        self.assertIn('"google.co.uk"', result)
        self.assertIn('case internal::ComputeAnnotationHash("metrics_report_uma"):', result)
        self.assertIn('return IsByodGoogleServiceEndpoint(url);', result)
        self.assertIn('default:\n      return false;', result)

    def test_render_rejects_invalid_domain(self):
        with self.assertRaises(ValueError):
            guard.render_header(set(), ['bad"domain'])

    def test_google_tlds_do_not_include_youtube_only_domains(self):
        source = ('#define GOOGLE_TLD_LIST "com", "co.uk"\n'
                  '#define YOUTUBE_TLD_LIST "example", "test"\n')
        self.assertEqual(guard.google_tld_values(source), ["com", "co.uk"])


if __name__ == "__main__":
    unittest.main()
