#!/usr/bin/env python3
"""Audit a completed, isolated Chromium NetLog against the service manifest.

This verifies only requests actually present in the capture, not every browser
service or code path. Capture without flags that disable background networking,
then close the isolated browser normally before reading the JSON. Keep captures
private: unlike this report, raw NetLogs can contain URLs and credentials.
"""

import argparse
import json
from pathlib import Path
from urllib.parse import urlsplit


def audit(capture, manifest, required_hosts):
    constants = capture["constants"]
    event_names = {value: key for key, value in constants["logEventTypes"].items()}
    phases = constants["logEventPhase"]
    begin, end = phases["PHASE_BEGIN"], phases["PHASE_END"]
    rules = {}
    for entry in manifest["blocked"]:
        rule = (entry["id"], entry["google_endpoint_only"])
        previous = rules.setdefault(entry["hash"], rule)
        if previous != rule:
            raise ValueError("Conflicting annotation hashes in manifest")
    domains = manifest["google_service_domains"]
    requests, jobs, problems = {}, [], []

    def host(url):
        return (urlsplit(url).hostname or "").lower().rstrip(".")

    def guarded(annotation, url):
        rule = rules.get(annotation)
        if not rule:
            return False
        name = host(url)
        return not rule[1] or any(
            name == domain or name.endswith("." + domain) for domain in domains
        )

    for event in capture["events"]:
        event_name = event_names.get(event["type"])
        source = event["source"]["id"]
        phase = event["phase"]
        params = event.get("params", {})
        if event_name == "REQUEST_ALIVE" and phase == begin:
            if "traffic_annotation" not in params:
                raise ValueError("Capture is missing request traffic annotations")
            requests[source] = {"annotation": params["traffic_annotation"], "job": None}
        elif event_name == "URL_REQUEST_START_JOB":
            if source not in requests:
                raise ValueError("Capture starts after a request was created")
            request = requests[source]
            if phase == begin:
                annotation = request["annotation"]
                job = {"source": source, "annotation": annotation,
                       "host": host(params["url"]),
                       "guarded": guarded(annotation, params["url"]),
                       "ended": False, "net_error": None, "network_events": 0}
                request["job"] = job
                jobs.append(job)
            elif phase == end:
                if request["job"] is None:
                    raise ValueError("Request job ends without a start event")
                request["job"]["ended"] = True
                request["job"]["net_error"] = params.get("net_error", 0)
        elif source in requests and requests[source]["job"] is not None:
            network_event = event_name == "HTTP_TRANSACTION_SEND_REQUEST_HEADERS"
            if event_name in ("URL_REQUEST_JOB_BYTES_READ",
                              "URL_REQUEST_JOB_FILTERED_BYTES_READ"):
                network_event = params.get("byte_count", 0) > 0
            if network_event:
                requests[source]["job"]["network_events"] += 1

    guarded_jobs = [job for job in jobs if job["guarded"]]
    for job in guarded_jobs:
        if (not job["ended"] or job["net_error"] != -20 or
                job["network_events"]):
            problems.append({"source": job["source"], "host": job["host"],
                             "annotation": rules[job["annotation"]][0],
                             "ended": job["ended"], "net_error": job["net_error"],
                             "network_events": job["network_events"]})
    successful_hosts = sorted({job["host"] for job in jobs
                               if not job["guarded"] and job["ended"]
                               and job["net_error"] == 0})
    missing_hosts = sorted(set(required_hosts) - set(successful_hosts))
    if not jobs:
        raise ValueError("Capture contains no URL request jobs")
    return {
        "scope": "observed URLRequest jobs only; unexercised services are not verified",
        "request_jobs": len(jobs), "guarded_jobs_seen": len(guarded_jobs),
        "guarded_jobs_blocked": len(guarded_jobs) - len(problems),
        "guard_coverage": "observed" if guarded_jobs else "not-exercised",
        "violations": problems, "missing_successful_hosts": missing_hosts,
        "required_hosts": sorted(set(required_hosts)),
        "observed_capture_passed": not problems and not missing_hosts,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("netlog", type=Path)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[1]
                        / "patches/google-services/google-services-manifest.json")
    parser.add_argument("--require-host", action="append", required=True,
                        help="Host explicitly visited by the isolated test; repeatable")
    args = parser.parse_args()
    report = audit(json.loads(args.netlog.read_text(encoding="utf-8")),
                   json.loads(args.manifest.read_text(encoding="utf-8")),
                   args.require_host)
    print(json.dumps(report, indent=2))
    return 0 if report["observed_capture_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
