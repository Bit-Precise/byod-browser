@echo off
setlocal
cd /d C:\src\chromium\src || exit /b 2
if not exist .git\rebase-merge\git-rebase-todo exit /b 3
for /f %%H in ('git rev-parse HEAD') do if not "%%H"=="cfaadc5a132d78e1828635aa8405a499f3e14864" exit /b 4
rem The deleted Polymer template is preserved in the source snapshot and git.
rem Its Connect control was ported to the new upstream Lit template.
call git rm chrome/browser/resources/settings/people_page/people_page.html
if errorlevel 1 exit /b 5
call git add chrome/browser/resources/settings/people_page/people_page.html.ts chrome/browser/resources/settings/people_page/people_page.ts chrome/browser/ui/browser_actions.cc chrome/browser/ui/browser_command_controller.cc chrome/browser/ui/startup/infobar_utils.cc chrome/browser/ui/views/frame/browser_view.cc chrome/browser/ui/webui/BUILD.gn chrome/browser/ui/webui_browser/webui_browser_exclusive_access_context.cc chrome/chrome_paks.gni services/network/network_context.cc
if errorlevel 1 exit /b 6
call git diff --cached --check
if errorlevel 1 exit /b 7
call git -c diff.ignoreSubmodules=all -c user.name=BYOD -c user.email=build@byod.invalid -c core.editor=true rebase --continue
if errorlevel 1 exit /b 8
call git log -1 --format=oneline
exit /b %errorlevel%
