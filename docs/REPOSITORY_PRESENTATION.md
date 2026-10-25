# Repository discoverability and brand assets

## GitHub About (manual repository setting)

Suggested description:

> Open-source document and graph database with NexoraQL, a C++20/RocksDB engine, Python API, and web dashboard. Experimental research project.

Suggested topics (only supported capabilities):

`nexoradb`, `nexoraql`, `database`, `document-database`, `graph-database`,
`embedded-database`, `rocksdb`, `cpp`, `python`, `query-language`, `open-source`.

These settings are not changed by editing the README. No ranking or performance
superiority is promised. Keep the experimental status visible.

GitHub social preview is uploaded separately in Settings > Social preview;
use a 1280 x 640 PNG/JPG under 1 MB, not the four-variant brand reference sheet.
See [GitHub guidance](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/customizing-your-repositorys-social-media-preview).

## Logo usage

Sources: `frontend/public/brand/`. SVGs are a project-local vector interpretation
of the owner's selected reference, not an exact extraction of its typography.

- `nexoradb-logo.svg`: full color wordmark on light backgrounds; README and sidebar.
- `nexoradb-logo-dark.svg`: light lettering for dark backgrounds.
- `nexoradb-icon.svg`: symbol without lettering for compact uses.
- `nexoradb-logo-mono.svg`, `nexoradb-icon-mono.svg`: monochrome printing and restricted palettes; invert on dark surfaces.
- `frontend/public/favicon.svg`: icon-only mark on navy, suitable for browser tabs.

Keep proportions and clear space. Do not use a full wordmark at favicon size.
Dashboard build copies assets into the packaged static directory. Rebuild after changes.

The administration dashboard deliberately uses `noindex, nofollow`: public repo
discoverability should not expose private console pages. This is not access control.
