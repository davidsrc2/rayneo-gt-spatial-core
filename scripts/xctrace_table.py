#!/usr/bin/env python3
"""Counts of an `xctrace export --xpath ...table[@schema=...]` XML by some of its columns.

    scripts/xctrace_table.py TABLE.xml COLUMN [COLUMN ...]

Each row's cells as their display strings (xctrace's id / ref deduplication resolved).
Example: displayed-surfaces-interval.xml display-name direct-to-display detachment-reason
"""
import sys, xml.etree.ElementTree as ET, collections
def rows(path):
    root = ET.parse(path).getroot()
    node = root.find('.//node')
    cols = [c.find('mnemonic').text for c in node.find('schema').findall('col')]
    byid = {}
    for r in node.findall('row'):
        vals = []
        for el in list(r):
            if 'ref' in el.attrib:
                el = byid.get(el.attrib['ref'], el)
            elif 'id' in el.attrib:
                byid[el.attrib['id']] = el
            vals.append((el.attrib.get('fmt') or (el.text or '')).strip() if el.tag != 'sentinel' else '')
        yield dict(zip(cols, vals))
if __name__ == '__main__':
    t = sys.argv[1]
    keys = sys.argv[2:]
    c = collections.Counter(tuple(r.get(k, '') for k in keys) for r in rows(t))
    for k, n in c.most_common(25): print(n, k)
