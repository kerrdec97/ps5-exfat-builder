#include "core/RandomSource.h"

#include <algorithm>

namespace fw {

SourceDevice::SourceDevice(std::shared_ptr<RandomSource> source, QObject* parent)
    : QIODevice(parent), m_source(std::move(source))
{
}

qint64 SourceDevice::size() const
{
    return m_source ? m_source->size() : 0;
}

qint64 SourceDevice::readData(char* data, qint64 maxSize)
{
    if (!m_source)
        return -1;
    const qint64 at = pos();
    const qint64 n = std::min(maxSize, m_source->size() - at);
    if (n <= 0)
        return 0;
    const Status st = m_source->readAt(at, data, n);
    if (!st.ok()) {
        setErrorString(st.error().message);
        return -1;
    }
    return n;
}

std::unique_ptr<SourceDevice> openSourceDevice(std::shared_ptr<RandomSource> source)
{
    auto dev = std::make_unique<SourceDevice>(std::move(source));
    dev->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    return dev;
}

} // namespace fw
