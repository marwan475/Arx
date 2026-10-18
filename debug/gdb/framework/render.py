"""Bounded generic DWARF rendering; pointer targets are never followed."""
import gdb

from .results import ReadFailure, Result


def gather(value, budget, source=0):
    result = Result(source=source, type_name=str(value.type),
                    count_semantics="fields/elements rendered from copied record bytes")

    def visit(current, path, depth):
        if depth > budget.depth:
            raise ReadFailure("truncated: depth limit")
        value_type = current.type.strip_typedefs()
        if value_type.code in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_UNION):
            budget.take_entry()
            if value_type.code == gdb.TYPE_CODE_UNION:
                result.fail("union interpretations overlap; no active-member guarantee")
            for field in value_type.fields():
                if field.is_base_class or field.name is None:
                    result.fail("unsupported anonymous/base field")
                    continue
                visit(current[field.name], path + ("." if path else "") + field.name,
                      depth + 1)
        elif value_type.code == gdb.TYPE_CODE_ARRAY:
            budget.take_entry()
            low, high = value_type.range()
            for index in range(low, high + 1):
                visit(current[index], "%s[%d]" % (path, index), depth + 1)
        else:
            budget.take_entry()
            if value_type.code == gdb.TYPE_CODE_PTR:
                text = "%#x (pointer; not expanded)" % int(current)
            elif value_type.code == gdb.TYPE_CODE_ENUM:
                number = int(current)
                names = [field.name for field in value_type.fields()
                         if field.enumval == number]
                text = "%s (%d)" % ("|".join(names) or "unknown", number)
            elif value_type.code == gdb.TYPE_CODE_FLT:
                text = str(float(current))
            elif value_type.code in (gdb.TYPE_CODE_INT, gdb.TYPE_CODE_BOOL,
                                     gdb.TYPE_CODE_CHAR):
                text = str(int(current))
            else:
                text = "<unsupported type %s>" % value_type
                result.fail("unsupported field type")
            result.items.append({"path": path or "value", "type": str(value_type),
                                 "text": text})

    try:
        visit(value, "", 0)
    except (ReadFailure, gdb.error, ValueError) as error:
        result.fail(error)
    return result


def expand(result, budget):
    """Finish gathering collection fields before presentation starts."""
    for item in result.items:
        if "value" not in item:
            continue
        fields = gather(item.pop("value"), budget, item["address"])
        item["fields"] = fields.items
        for issue in fields.issues:
            result.fail(issue)
    return result


def present(result, budget):
    gdb.write("source=%#x type=%s; %s; units=%s\n" %
              (result.source, result.type_name, result.count_semantics, result.units))
    for item in result.items:
        if "path" in item:
            gdb.write("  %s: %s [%s]\n" % (item["path"], item["text"], item["type"]))
        elif "text" in item:
            gdb.write("  %#x: %s\n" % (item["address"], item["text"]))
        else:
            labels = " ".join("%s=%s" % (key, val) for key, val in item.items()
                              if key not in ("address", "fields"))
            gdb.write("  %#x %s\n" % (item["address"], labels))
            for field in item.get("fields", []):
                gdb.write("    %s: %s [%s]\n" %
                          (field["path"], field["text"], field["type"]))
    gdb.write("%s; entries=%d bytes=%d\n" %
              (result.status, budget.used_entries, budget.used_bytes))
    for issue in result.issues:
        gdb.write("  %s\n" % issue)
