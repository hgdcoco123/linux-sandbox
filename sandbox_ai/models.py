"""Small, versioned data contracts shared by adapters, retrieval and analysis."""
from dataclasses import asdict, dataclass, field
import hashlib
import json


class AnalysisError(RuntimeError):
    pass


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


@dataclass(frozen=True)
class Evidence:
    id: str
    scope: str
    run_id: str
    kind: str
    source: str
    location: str
    text: str
    truncated: bool = False

    def json(self):
        return asdict(self)


def evidence(scope, run_id, kind, source, location, text, truncated=False):
    body = dict(scope=scope, run_id=run_id, kind=kind, source=source,
                location=location, text=text, truncated=truncated)
    return Evidence(id='ev-' + digest(body)[:24], **body)


@dataclass
class Bundle:
    scope: str
    run_id: str
    execution: dict
    evidence: list[Evidence]
    warnings: list[str] = field(default_factory=list)
    # Where this snapshot came from. Recorded so a *bounded* range read of a captured
    # stream (stdout/stderr/internal log) can be offered later as a tool; empty for
    # bundles imported before the field existed, which simply disables that tool.
    run_dir: str = ''

    @property
    def fingerprint(self):
        return digest(self.json())

    def json(self):
        return dict(schema_version=2, scope=self.scope, run_id=self.run_id,
                    execution=self.execution, evidence=[e.json() for e in self.evidence],
                    warnings=self.warnings, run_dir=self.run_dir)

    @classmethod
    def load(cls, value):
        version = value.get('schema_version')
        if version not in (1, 2):
            raise AnalysisError('Unsupported evidence schema')
        return cls(value['scope'], value['run_id'], value['execution'],
                   [Evidence(**item) for item in value['evidence']], value['warnings'],
                   value.get('run_dir', ''))
