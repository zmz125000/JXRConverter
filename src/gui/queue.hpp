#pragma once
#include "jxrc/converter.hpp"
#include <QObject>
#include <QHash>
#include <QSet>
#include <QThreadPool>
#include <QString>
#include <QVector>
#include <functional>

namespace jxrc::gui {
struct Job { QString input, output; Options options; };
int coreCount();
int recommendedWorkers(int cores);
struct OutputLayout {
    bool format_subfolders = false;
    bool source_folder = false;
    bool rename_conflicts = false;
    QSet<QString> raw_inputs; // Catalog-identified RAW, including TIFF sensor containers.
};
QVector<Job> makeJobs(const QStringList& inputs, const QString& directory, const Options& options, const OutputLayout& layout = {});
class ConversionQueue : public QObject {
    Q_OBJECT
public:
    using Convert = std::function<Result(const std::filesystem::path&, const std::filesystem::path&, const Options&)>;
    explicit ConversionQueue(QObject* parent = nullptr, Convert converter = jxrc::convert);
    ~ConversionQueue() override;
    void start(QVector<Job> jobs, int parallelism, int cores = coreCount());
    void cancelPending();
    bool busy() const { return active_ > 0 || next_ < jobs_.size(); }
    int total() const { return jobs_.size(); }
signals:
    void statusChanged(const QString& path, const QString& state, const QString& detail);
    void progress(int finished, int total, int active);
    void batchFinished(int succeeded, int failed, int cancelled);
private:
    void pump();
    void finishFile(const Job& job, const QString& error, bool cancelled);
    QVector<Job> jobs_;
    int next_ = 0, active_ = 0, finished_ = 0, succeeded_ = 0, failed_ = 0, cancelled_ = 0, limit_ = 1;
    QThreadPool pool_;
    Convert converter_;
    QHash<QString, int> remaining_, fileCancelled_;
    QHash<QString, QString> fileErrors_;
};
}
