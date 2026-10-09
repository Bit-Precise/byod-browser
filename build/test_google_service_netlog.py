import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "netlog", Path(__file__).with_name("verify-google-service-netlog.py"))
netlog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netlog)
MANIFEST = {"blocked": [
    {"id": "metrics", "hash": 10, "google_endpoint_only": False},
    {"id": "shared", "hash": 11, "google_endpoint_only": True},
], "google_service_domains": ["google.com", "google.co.uk"]}
TYPES = {"REQUEST_ALIVE": 1, "URL_REQUEST_START_JOB": 2,
         "HTTP_TRANSACTION_SEND_REQUEST_HEADERS": 3}


def event(kind, source, phase, **params):
    return {"type": TYPES[kind], "source": {"id": source},
            "phase": phase, "params": params}


def request(source, annotation, url, error=0):
    return [event("REQUEST_ALIVE", source, 1, url=url, traffic_annotation=annotation),
            event("URL_REQUEST_START_JOB", source, 1, url=url),
            event("URL_REQUEST_START_JOB", source, 2, net_error=error)]


def check(events, hosts=()):
    return netlog.audit({"constants": {"logEventTypes": TYPES,
                        "logEventPhase": {"PHASE_BEGIN": 1, "PHASE_END": 2}},
                        "events": events}, MANIFEST, hosts)


class NetLogTests(unittest.TestCase):
    def test_denied_service(self):
        report = check(request(1, 10, "https://service.test/path", -20))
        self.assertTrue(report["observed_capture_passed"])
        self.assertEqual(report["guarded_jobs_blocked"], 1)

    def test_service_success_is_failure(self):
        self.assertFalse(check(request(1, 10, "https://service.test/"))[
            "observed_capture_passed"])

    def test_dns_error_does_not_prove_guard(self):
        self.assertFalse(check(request(1, 10, "https://service.test/", -105))[
            "observed_capture_passed"])

    def test_unfinished_service_is_failure(self):
        self.assertFalse(check(request(1, 10, "https://service.test/")[:-1])[
            "observed_capture_passed"])

    def test_ordinary_google_website_is_not_a_service(self):
        report = check(request(1, 42, "https://www.google.com/"), ["www.google.com"])
        self.assertTrue(report["observed_capture_passed"])
        self.assertEqual(report["guard_coverage"], "not-exercised")

    def test_shared_service_non_google_destination_is_allowed(self):
        self.assertTrue(check(request(1, 11, "https://updates.example/"))[
            "observed_capture_passed"])

    def test_shared_service_regional_google_destination_is_blocked(self):
        self.assertFalse(check(request(1, 11, "https://www.google.co.uk/"))[
            "observed_capture_passed"])

    def test_domain_boundary(self):
        self.assertTrue(check(request(1, 11, "https://notgoogle.com/"))[
            "observed_capture_passed"])

    def test_mixed_service_redirect_to_google_checked_per_job(self):
        events = request(1, 11, "https://updates.example/")
        events += [event("URL_REQUEST_START_JOB", 1, 1, url="https://google.com/"),
                   event("URL_REQUEST_START_JOB", 1, 2, net_error=0)]
        self.assertEqual(len(check(events)["violations"]), 1)

    def test_network_activity_before_block_is_failure(self):
        events = request(1, 10, "https://service.test/", -20)
        events.insert(2, event("HTTP_TRANSACTION_SEND_REQUEST_HEADERS", 1, 0))
        self.assertFalse(check(events)["observed_capture_passed"])

    def test_missing_positive_control_is_failure(self):
        self.assertFalse(check(request(1, 10, "https://service.test/", -20),
                               ["www.google.com"])["observed_capture_passed"])

    def test_empty_capture_is_not_success(self):
        with self.assertRaises(ValueError):
            check([])

    def test_capture_missing_annotations_is_rejected(self):
        with self.assertRaises(ValueError):
            check([event("REQUEST_ALIVE", 1, 1, url="https://example.test/")])

    def test_report_does_not_print_url_secrets(self):
        report = check(request(1, 10, "https://user:password@service.test/token?q=secret#s"))
        self.assertNotIn("password", str(report))
        self.assertNotIn("secret", str(report))


if __name__ == "__main__":
    unittest.main()
