# SPDX-License-Identifier: GPL-2.0-or-later
"""
QAPI MCP tool table generator

Every command with the feature 'mcp' becomes an MCP tool: its name,
a plain-text description from the doc comment, a JSON Schema for its
arguments, and annotations from the features 'mcp-read-only' and
'mcp-destructive'.
"""

import json
import re
from typing import Dict, List, Optional

from .common import c_name, mcgen
from .gen import QAPISchemaMonolithicCVisitor
from .parser import QAPIDoc
from .schema import (
    QAPISchema,
    QAPISchemaAlternateType,
    QAPISchemaArrayType,
    QAPISchemaBuiltinType,
    QAPISchemaEnumType,
    QAPISchemaFeature,
    QAPISchemaIfCond,
    QAPISchemaObjectType,
    QAPISchemaType,
)
from .source import QAPISourceInfo


_INT_RANGES = {
    'int8': (-2**7, 2**7 - 1),
    'int16': (-2**15, 2**15 - 1),
    'int32': (-2**31, 2**31 - 1),
    'uint8': (0, 2**8 - 1),
    'uint16': (0, 2**16 - 1),
    'uint32': (0, 2**32 - 1),
    'uint64': (0, None),
    'size': (0, None),
}


def plain_text(text: str) -> str:
    """Doc comment text as one plain line: no rST, no QAPI markup."""
    lines = []
    for line in text.split('\n'):
        if line.strip().startswith('.. '):
            break                       # an example or other directive
        lines.append(line.strip())
    text = ' '.join(lines)
    text = re.sub(r'@([\w-]+)', r'\1', text)
    text = re.sub(r'``([^`]*)``', r'\1', text)
    text = re.sub(r'`([^`]*)`', r'\1', text)
    text = re.sub(r'\s+', ' ', text)
    return text.strip()


def doc_default(text: str, schema_type: Dict[str, object]) -> object:
    """The value of '(default: X)' if X is a valid literal, else None."""
    m = re.search(r'\(default:\s*([^)]*)\)', text)
    if not m:
        return None
    value = m.group(1).strip()
    try:
        literal = json.loads(value)
    except ValueError:
        literal = value
    kind = schema_type.get('type')
    if kind == 'boolean' and isinstance(literal, bool):
        return literal
    if kind == 'integer' and isinstance(literal, int) \
            and not isinstance(literal, bool):
        return literal
    if kind == 'number' and isinstance(literal, (int, float)) \
            and not isinstance(literal, bool):
        return literal
    if kind == 'string' and literal in schema_type.get('enum', []):
        return literal
    return None


class QAPISchemaGenMcpVisitor(QAPISchemaMonolithicCVisitor):

    def __init__(self, prefix: str):
        super().__init__(
            prefix, 'qapi-mcp-tools',
            ' * MCP tool table',
            __doc__)
        self._docs: Dict[int, str] = {}
        self._command_docs: Dict[str, QAPIDoc] = {}
        self._count = 0

    def visit_begin(self, schema: QAPISchema) -> None:
        for doc in schema.docs:
            # pylint: disable=protected-access
            for section in doc._args.values():
                if section.member is not None:
                    self._docs[id(section.member)] = section.text
            if doc.symbol:
                self._command_docs[doc.symbol] = doc
        self._genc.add(mcgen('''
#include "qemu/osdep.h"
#include "monitor/mcp-tools.h"

const McpToolDef %(c_prefix)smcp_tools[] = {
''', c_prefix=c_name(self._prefix, protect=False)))

    def visit_end(self) -> None:
        self._genc.add(mcgen('''
    { NULL }
};
'''))

    def _type_schema(self, typ: QAPISchemaType,
                     seen: List[str]) -> Dict[str, object]:
        if isinstance(typ, QAPISchemaBuiltinType):
            if typ.name == 'any':
                return {}
            kind = typ.json_type()
            if kind == 'int':
                ret: Dict[str, object] = {'type': 'integer'}
                lo, hi = _INT_RANGES.get(typ.name, (None, None))
                if lo is not None:
                    ret['minimum'] = lo
                if hi is not None:
                    ret['maximum'] = hi
                return ret
            if kind == 'value':
                return {}
            return {'type': kind}
        if isinstance(typ, QAPISchemaEnumType):
            return {'type': 'string',
                    'enum': [m.name for m in typ.members]}
        if isinstance(typ, QAPISchemaArrayType):
            return {'type': 'array',
                    'items': self._type_schema(typ.element_type, seen)}
        if isinstance(typ, QAPISchemaAlternateType):
            return {'anyOf': [self._type_schema(v.type, seen)
                              for v in typ.alternatives.variants]}
        assert isinstance(typ, QAPISchemaObjectType)
        if typ.name in seen:
            return {'type': 'object'}       # recursive type: stop here
        seen = seen + [typ.name]
        ret = self._object_schema(typ, seen)
        if typ.branches:
            tag = typ.branches.tag_member.name
            choices = []
            for v in typ.branches.variants:
                var = self._object_schema(v.type, seen)
                props = dict(ret.get('properties', {}))
                props.update(var.get('properties', {}))
                props[tag] = {'const': v.name}
                choice: Dict[str, object] = {'type': 'object',
                                             'properties': props}
                required = list(ret.get('required', []))
                required += var.get('required', [])
                if required:
                    choice['required'] = required
                choices.append(choice)
            return {'oneOf': choices}
        return ret

    def _object_schema(self, typ: QAPISchemaObjectType,
                       seen: List[str]) -> Dict[str, object]:
        props: Dict[str, object] = {}
        required = []
        for m in typ.members:
            prop = self._type_schema(m.type, seen)
            text = self._docs.get(id(m))
            if text:
                default = doc_default(text, prop)
                prop = dict(prop)
                prop['description'] = plain_text(text)
                if default is not None:
                    prop['default'] = default
            props[m.name] = prop
            if not m.optional:
                required.append(m.name)
        ret: Dict[str, object] = {'type': 'object', 'properties': props}
        if required:
            ret['required'] = required
        return ret

    def _description(self, name: str) -> str:
        doc = self._command_docs.get(name)
        if not doc:
            return ''
        parts = [s.text for s in doc.all_sections
                 if s.kind in (QAPIDoc.Kind.INTRO, QAPIDoc.Kind.PLAIN)]
        return plain_text('\n'.join(parts))

    # pylint: disable=too-many-arguments
    def visit_command(self,
                      name: str,
                      info: Optional[QAPISourceInfo],
                      ifcond: QAPISchemaIfCond,
                      features: List[QAPISchemaFeature],
                      arg_type: Optional[QAPISchemaObjectType],
                      ret_type: Optional[QAPISchemaType],
                      gen: bool,
                      success_response: bool,
                      boxed: bool,
                      allow_oob: bool,
                      allow_preconfig: bool,
                      coroutine: bool) -> None:
        names = [f.name for f in features]
        if 'mcp' not in names:
            return
        read_only = 'mcp-read-only' in names
        destructive = 'mcp-destructive' in names
        if arg_type:
            schema = self._object_schema(arg_type, [arg_type.name])
        else:
            schema = {'type': 'object', 'properties': {}}
        annotations: Dict[str, object] = {}
        if read_only:
            annotations['readOnlyHint'] = True
        else:
            annotations['destructiveHint'] = destructive
            annotations['idempotentHint'] = False
        annotations['openWorldHint'] = False
        tool = {
            'name': name,
            'description': self._description(name),
            'inputSchema': schema,
            'annotations': annotations,
        }
        text = json.dumps(tool, separators=(',', ':'))
        literal = '\n'.join(
            '        ' + json.dumps(text[i:i + 70])
            for i in range(0, len(text), 70))
        self._genc.add(ifcond.gen_if())
        self._genc.add(mcgen('''
    {
        .name = "%(name)s",
        .read_only = %(read_only)s,
        .json =
%(literal)s,
    },
''', name=name, read_only='true' if read_only else 'false',
                             literal=literal))
        self._genc.add(ifcond.gen_endif())
        self._count += 1


def gen_mcp(schema: QAPISchema, output_dir: str, prefix: str) -> None:
    vis = QAPISchemaGenMcpVisitor(prefix)
    schema.visit(vis)
    vis.write(output_dir)
