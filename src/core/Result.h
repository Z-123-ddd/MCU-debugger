#pragma once

#include <QString>
#include <utility>

namespace mrd {

/// Simple success/failure carrier for operations that cannot fail with a value.
struct Status
{
    bool ok = true;
    QString error;

    static Status success() { return {true, QString()}; }
    static Status failure(const QString &msg) { return {false, msg}; }

    explicit operator bool() const { return ok; }

    /// Prepend context to an existing error ("open probe: <cause>").
    Status withContext(const QString &context) const
    {
        if (ok)
            return {false, context};
        return {false, context + QStringLiteral(": ") + error};
    }
};

/// Success/failure carrier carrying a value on success.
template <typename T>
struct Result
{
    bool ok = false;
    T value{};
    QString error;

    static Result success(T v)
    {
        Result r;
        r.ok = true;
        r.value = std::move(v);
        return r;
    }
    static Result failure(const QString &msg)
    {
        Result r;
        r.ok = false;
        r.error = msg;
        return r;
    }

    explicit operator bool() const { return ok; }
};

} // namespace mrd
