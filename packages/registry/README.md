# registry

Storage primitives for the Diamond cut registry.

This first slice provides the SQLite schema and a content-addressed blob store.
The HTTP service, authentication flow, archive validation, and publish
transaction will build on these primitives.

Install with `facet` and load with `require_cut "registry"`.
