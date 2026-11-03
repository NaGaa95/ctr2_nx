"""Host regression checks; no game data or Switch hardware is required."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def production_function(path, name):
    text = path.read_text(encoding="utf-8")
    start = text.index("int " + name + "(")
    opening = text.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    compiler = shlex.split(os.environ["CC"]) if "CC" in os.environ else [
        next((p for name in ("cc", "clang", "gcc") if (p := shutil.which(name))), "cc")]
    with tempfile.TemporaryDirectory(prefix="ctr-host-checks-", ignore_cleanup_errors=True) as temp:
        work = Path(temp)
        def run(name, sources):
            exe = work / (name + (".exe" if os.name == "nt" else ""))
            subprocess.run(compiler + ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                           "-I" + str(ROOT / "source")] +
                           [str(p) for p in sources] + ["-o", str(exe)], check=True)
            subprocess.run([str(exe)], cwd=work, check=True)
        run("config", [ROOT / "tests" / "test_config.c", ROOT / "source" / "config.c"])
        subscription_test = ROOT / "tests" / "test_subscription.c"
        if subscription_test.is_file():
            function = production_function(ROOT / "source" / "jni_fake.c",
                                           "jni_offline_subscription_tap")
            generated = work / "subscription.c"
            template = subscription_test.read_text(encoding="utf-8")
            generated.write_text(template.replace("/* PRODUCTION_SUBSCRIPTION_TAP */", function),
                                 encoding="utf-8")
            run("subscription", [generated])


if __name__ == "__main__":
    main()
