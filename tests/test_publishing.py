"""Exercise publication boundaries without credentials or network access."""

import sys
from pathlib import Path
from unittest import TestCase, mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import publish_github as publishing


class PublishingChecks(TestCase):
    def test_wrong_account_stops_before_any_repository_write(self):
        with mock.patch.object(publishing.shutil, "which", return_value="gh"), \
                mock.patch.object(publishing, "api", return_value={"login": "example-account"}), \
                mock.patch.object(publishing, "run") as run:
            with self.assertRaises(RuntimeError):
                publishing.publish()
            run.assert_not_called()

    def test_existing_repository_must_be_the_public_destination(self):
        repository = {"full_name": publishing.REPOSITORY, "private": False,
                      "visibility": "public", "fork": False, "archived": False, "has_issues": True}
        publishing.validate_repository(repository)
        for changed in ({"private": True}, {"visibility": "private"}, {"fork": True},
                        {"full_name": "example/another-repository"}, {"archived": True}, {"has_issues": False}):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                publishing.validate_repository(dict(repository, **changed))

    def test_separate_push_url_cannot_send_source_elsewhere(self):
        for push_urls in ("https://example.test/another.git", publishing.REMOTE_URL + "\nhttps://example.test/another.git"):
            with mock.patch.object(publishing, "git", side_effect=["origin", publishing.REMOTE_URL, push_urls]):
                with self.assertRaises(RuntimeError):
                    publishing.check_remote()

    def test_issue_retry_preserves_closed_issues_and_user_edits(self):
        drafts = publishing.issue_drafts()
        existing = [{"body": marker + "\nUpdated by a user", "state": "closed",
                     "html_url": publishing.URL + f"/issues/{i + 1}"}
                    for i, (_, _, marker) in enumerate(drafts)]
        with mock.patch.object(publishing, "api", return_value=existing), \
                mock.patch.object(publishing, "run") as run, mock.patch("builtins.print"):
            publishing.seed_issues()
            run.assert_not_called()

    def test_issue_creation_uses_reviewed_body_files_and_handles_pagination(self):
        draft = publishing.issue_drafts()[0]
        pull_requests = [{"pull_request": {}, "body": draft[2]}] * 100
        with mock.patch.object(publishing, "api", side_effect=[pull_requests, []]) as api, \
                mock.patch.object(publishing, "issue_drafts", return_value=[draft]), \
                mock.patch.object(publishing, "run") as run:
            publishing.seed_issues()
            self.assertIn("page=2", api.call_args.args[0])
            run.assert_called_once_with("gh", "issue", "create", "--repo", "github.com/" + publishing.REPOSITORY,
                                        "--title", draft[1], "--body-file", str(draft[0]), visible=True)
