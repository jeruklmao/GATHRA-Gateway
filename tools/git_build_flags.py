Import("env")

import subprocess


def git_revision():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except Exception:
        return "unknown"


env.Append(CPPDEFINES=[("GATHRA_GIT_COMMIT", env.StringifyMacro(git_revision()))])
