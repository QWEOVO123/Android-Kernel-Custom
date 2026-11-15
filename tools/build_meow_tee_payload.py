"""Build the Meow boot Java payload and reuse the matching release's ARM64 tools."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile


RELEASE_SHA256 = "ea4cfad7312834b4843e1fe2d7107e4d587e8c988eca558e0656985043928051"
NATIVE_FILES = {
    "libTEESimulator.so": "libTEESimulator.so",
    "libcertgen.so": "libcertgen.so",
    "libinject.so": "inject",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("release", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument('--android-sdk', type=Path, default=os.getenv('ANDROID_HOME'),
                        required=not os.getenv('ANDROID_HOME'))
    parser.add_argument('--java-home', type=Path, default=os.getenv('JAVA_HOME'),
                        required=not os.getenv('JAVA_HOME'))
    parser.add_argument('--gradle-cache', type=Path,
                        default=Path(os.getenv('GRADLE_USER_HOME', Path.home() / '.gradle'))
                        / 'caches/modules-2/files-2.1')
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    if output.exists():
        parser.error("output already exists")
    if hashlib.sha256(args.release.read_bytes()).hexdigest() != RELEASE_SHA256:
        parser.error("upstream release digest mismatch")
    cache = args.gradle_cache.resolve()
    sdk = args.android_sdk.resolve()
    jdk = args.java_home.resolve() / 'bin'
    exe = '.exe' if os.name == 'nt' else ''
    android = sdk / "platforms/android-37.0/android.jar"

    def jar(group, artifact, version):
        return next((cache / group / artifact / version).glob(f"*/{artifact}-{version}.jar"))

    def classpath(paths):
        return os.pathsep.join(map(str, paths))

    def compile_with(command, options, stage, argfile):
        file = stage / argfile
        file.write_text("\n".join(f'"{str(value).replace(chr(92), "/")}"' for value in options),
                        encoding="utf-8")
        subprocess.run(command + ["@" + str(file)], check=True)

    kotlin_version = "2.2.10"
    stdlib = jar("org.jetbrains.kotlin", "kotlin-stdlib", kotlin_version)
    compiler = jar("org.jetbrains.kotlin", "kotlin-compiler-embeddable", kotlin_version)
    coroutines = jar("org.jetbrains.kotlinx", "kotlinx-coroutines-core-jvm", "1.8.0")
    annotations = jar("org.jetbrains", "annotations", "23.0.0")
    androidx = sorted(p for p in (cache / "androidx.annotation/annotation-jvm").glob("*/*/*.jar")
                      if not p.name.endswith("-sources.jar"))[-1]
    bc = [jar("org.bouncycastle", n, "1.79") for n in
          ("bcprov-jdk18on", "bcpkix-jdk18on", "bcutil-jdk18on")]

    with TemporaryDirectory() as tmp:
        stage = Path(tmp)
        stubs = stage / "stubs"
        classes = stage / "classes"
        dex = stage / "dex"
        dex.mkdir()
        build_config = stage / "BuildConfig.java"
        build_config.write_text(
            "package org.matrix.TEESimulator; public final class BuildConfig {"
            " public static final boolean DEBUG = false; }", encoding="utf-8")
        compile_with([str(jdk / ("javac" + exe))],
                     ["-encoding", "UTF-8", "--release", "21", "-cp",
                      classpath([android, androidx, annotations, stdlib]),
                      "-d", stubs, build_config, *sorted(source.glob("stub/src/main/java/**/*.java"))],
                     stage, "javac.args")
        compile_with([str(jdk / ("java" + exe)), "-cp",
                      classpath([compiler, stdlib, coroutines, annotations]),
                      "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler"],
                     ["-no-stdlib", "-no-reflect", "-jvm-target", "21", "-classpath",
                      classpath([android, stubs, stdlib, androidx, annotations, *bc]),
                      "-d", classes, *sorted(source.glob("app/src/main/java/**/*.kt"))],
                     stage, "kotlinc.args")
        app_jar = stage / "classes.jar"
        with ZipFile(app_jar, "w", ZIP_DEFLATED) as archive:
            for file in sorted(classes.rglob("*.class")):
                archive.write(file, file.relative_to(classes).as_posix())
            archive.write(stubs / "org/matrix/TEESimulator/BuildConfig.class",
                          "org/matrix/TEESimulator/BuildConfig.class")
        # Use only the unversioned Bouncy Castle classes on Android.
        runtime = stage / "crypto.jar"
        with ZipFile(runtime, "w", ZIP_DEFLATED) as archive:
            for dependency in bc:
                with ZipFile(dependency) as input_jar:
                    for name in input_jar.namelist():
                        if name.startswith("org/") and name.endswith(".class"):
                            archive.writestr(name, input_jar.read(name))
        d8 = sdk / "build-tools/37.0.0/lib/d8.jar"
        subprocess.run([str(jdk / ("java" + exe)), "-cp", str(d8), "com.android.tools.r8.D8",
                        "--release", "--min-api", "29", "--lib", str(android),
                        "--classpath", str(stubs), "--output", str(dex),
                        str(app_jar), str(stdlib), str(runtime)], check=True)
        service = stage / "service.jar"
        dex_files = sorted(dex.glob("classes*.dex"))
        if not dex_files or not any(b"Lorg/matrix/TEESimulator/App;" in f.read_bytes() for f in dex_files):
            raise RuntimeError("DEX entry point missing")
        with ZipFile(service, "w", ZIP_DEFLATED) as archive:
            for file in dex_files:
                archive.write(file, file.name)
        archive_path = stage / "payload.tar.gz"
        with ZipFile(args.release) as upstream, tarfile.open(archive_path, "w:gz") as archive:
            for upstream_name, name in NATIVE_FILES.items():
                data = upstream.read(f"lib/arm64-v8a/{upstream_name}")
                if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], "little") != 183:
                    raise ValueError(f"{upstream_name}: expected AArch64 ELF")
                entry = tarfile.TarInfo(name)
                entry.mode = 0o700 if name == "inject" else 0o600
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
            archive.add(service, arcname="service.jar")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(archive_path.read_bytes())
    print(json.dumps({"payload_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
                      "payload_bytes": output.stat().st_size,
                      "upstream_native_sha256": RELEASE_SHA256,
                      "dex_files": len(dex_files)}, indent=2))


if __name__ == "__main__":
    main()
