# Request cut maintainership

To publish a new cut or become a maintainer of an existing cut on
[cuts.dilang.tech](https://cuts.dilang.tech),
[open a maintainership request](https://github.com/diamond-language/diamond/issues/new?template=cut-maintainership.md)
in `diamond-language/diamond`. A GitHub account is required. This is a public,
operator-reviewed request; submitting it does not grant access or reserve a name.

## What to include

- Exact cut names and whether this is a new publication, additional maintainer,
  or ownership transfer.
- Source repository URL and proposed release tag/commit, if publishing.
- Your GitHub identity, relationship to the project, and evidence of source
  maintainership. For existing cuts, identify the current owner and link their
  approval where available.
- Requested access: publishing only, or also managing releases and owners;
  explain the need and whether access is temporary or ongoing.
- For new cuts, the license and results of `facet check`, `facet pack`, and
  `facet verify`. Follow the [cut contract](cut-contract.md).

Keep tokens, credential files, private keys, and private contact details out of
issues and attachments. No credential is delivered in an issue comment.

## Review and access

1. An operator checks the source, package identity, name availability, and the
   requester's authority. Existing-name changes require verification with the
   current owners; a request alone is not evidence of ownership.
2. The operator records the decision and approved names/access in the issue.
   They agree a private credential delivery channel with the verified requester
   before issuing credentials. No response time is guaranteed.
3. The operator issues an expiring credential with only the approved name scopes:
   `publish:<name>` for publishing, and `manage:<name>` only when management is
   approved. Registry-wide `admin` access is not part of ordinary maintainership.
4. For existing cuts, the operator or an authorized current owner adds the exact
   credential subject as an owner. Ownership and credential issuance are separate
   operations. For a new name, the first authorized publication creates ownership.
5. The operator records completion without exposing credentials. Ownership and
   credential changes retain the registry's audit trail; reference the request
   in the administrative reason. Rotate or revoke credentials when access ends.

For access changes or transfers, use the same template and identify the existing
cut and affected maintainer. An ownership change does not rewrite published
versions or restore taken-down releases.

Operators: see [credential and ownership administration](../applications/registry/README.md#local-credential-administration)
and the [registry protocol](registry-protocol.md). Ordinary maintainers should
never receive server/database access to complete this process.
