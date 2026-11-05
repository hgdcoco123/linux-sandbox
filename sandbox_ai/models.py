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

    @property
    def fingerprint(self):
        return digest(self.json())

    def json(self):
        return dict(schema_version=1, scope=self.scope, run_id=self.run_id,
                    execution=self.execution, evidence=[e.json() for e in self.evidence],
                    warnings=self.warnings)

    @classmethod
    def load(cls, value):
        if value.get('schema_version') != 1:
            raise AnalysisError('Unsupported evidence schema')
        return cls(value['scope'], value['run_id'], value['execution'],
                   [Evidence(**item) for item in value['evidence']], value['warnings'])
