const webInstaller = document.querySelector("esp-web-install-button");
const firmwareVersion = document.querySelector("#firmware-version");
const releasePicker = document.querySelector(".release-picker");
const releaseSelect = document.querySelector("#firmware-release");
const includeBeta = document.querySelector("#include-beta");

let releases = [];

function selectRelease(tag) {
  const release = releases.find((candidate) => candidate.tag === tag);
  if (!release) return;

  webInstaller.setAttribute("manifest", release.manifest);
  firmwareVersion.textContent = `v${release.version}`;
  firmwareVersion.dataset.state = "ready";
}

function renderReleaseOptions(preferredTag) {
  const visible = releases.filter(
    (release) => includeBeta.checked || !release.prerelease,
  );
  releaseSelect.replaceChildren(
    ...visible.map((release) => {
      const option = document.createElement("option");
      option.value = release.tag;
      option.textContent = `v${release.version}${release.prerelease ? " (beta)" : ""}`;
      return option;
    }),
  );

  const selected = visible.find((release) => release.tag === preferredTag) || visible[0];
  if (selected) {
    releaseSelect.value = selected.tag;
    selectRelease(selected.tag);
  }
}

async function loadFirmwareVersion() {
  try {
    const response = await fetch("releases.json", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`Release catalog request failed with status ${response.status}`);
    }

    const catalog = await response.json();
    if (!Array.isArray(catalog.releases) || !catalog.releases.length) {
      throw new Error("Release catalog is empty");
    }

    releases = catalog.releases;
    renderReleaseOptions(catalog.default);
    releasePicker.disabled = false;
  } catch {
    // A single-build local artifact has no catalog, so retain its root manifest.
    try {
      const response = await fetch(webInstaller.getAttribute("manifest"), { cache: "no-store" });
      const manifest = await response.json();
      firmwareVersion.textContent = `v${manifest.version.trim().replace(/^v/, "")}`;
      firmwareVersion.dataset.state = "ready";
    } catch {
      firmwareVersion.textContent = "Version unavailable";
      firmwareVersion.dataset.state = "error";
    }
  }
}

releaseSelect.addEventListener("change", () => selectRelease(releaseSelect.value));
includeBeta.addEventListener("change", () => renderReleaseOptions(releaseSelect.value));

loadFirmwareVersion();
