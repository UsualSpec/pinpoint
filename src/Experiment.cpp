#include "Experiment.h"

#include "Sampler.h"
#include "Settings.h"

#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// Just required for proper formated abbrevations in iostream

using namespace units::power;
using namespace units::energy;
using namespace units::time;
using namespace units::edp;

struct ExperimentDetail
{
	using energy_series = std::vector<units::energy::joule_t>;
	using edp_series = std::vector<units::edp::joule_second_t>;

	std::vector<energy_series> energy_series_by_source;
	std::vector<edp_series> edp_series_by_source;
	std::vector<units::time::second_t> wall_times;

	void prepare(const size_t numSources, const size_t numRuns)
	{
		wall_times.clear();
		energy_series_by_source.clear();
		edp_series_by_source.clear();

		wall_times.reserve(numRuns);
		energy_series_by_source.resize(numSources);
		edp_series_by_source.resize(numSources);
	}

	void store_run(const Sampler::result_t & energy_by_source, const units::time::second_t & workload_wall_time)
	{
		wall_times.push_back(workload_wall_time);
		for (size_t i = 0; i < energy_by_source.size(); i++) {
			energy_series_by_source[i].push_back(energy_by_source[i]);
			edp_series_by_source[i].push_back(energy_by_source[i] * workload_wall_time);
		}
	}
};

Experiment::Experiment() :
	m_detail(new ExperimentDetail)
{
	;;
}

Experiment::~Experiment()
{
	delete m_detail;
}

void Experiment::run()
{
	m_detail->prepare(settings::counters.size(), settings::runs);

	for (unsigned int i = 0; i < settings::runs; i++) {
		if (settings::continuous_print_flag && settings::runs > 1)
			settings::output_stream << "### Run " << i << std::endl;
		run_single();
		std::this_thread::sleep_for(settings::delay);
	}
}

template<typename U>
std::tuple<units::unit_t<U>, double> meanAndStddevpercent(const std::vector<units::unit_t<U>> & values)
{
	using U_t = units::unit_t<U>;
	U_t mean(0);
	units::unit_t<units::squared<U>> variance(0);

	for (const U_t & value: values) {
		mean += value / settings::runs;
	}
	for (const U_t & value: values) {
		U_t vi = value - mean;
		variance += vi * vi / settings::runs;
	}

	U_t stddev = units::math::sqrt(variance);
	double stddev_percent = (stddev / mean) * 100.0;
	return std::make_tuple(mean, stddev_percent);
}

static constexpr size_t columnCount = 3; // value, source name, stddevpercent

template<typename U>
std::array<std::string, columnCount> formatSourceLine(const std::vector<units::unit_t<U>> & series, const std::string & sourceName)
{
	std::array<std::string, columnCount> columns;
	std::stringstream ss;

	auto mean = meanAndStddevpercent<U>(series);

	ss << std::fixed << std::setprecision(2) << std::get<0>(mean);
	columns[0] = ss.str();

	columns[1] = sourceName;

	ss.str(std::string()); ss.clear();

	ss << std::get<1>(mean); // Reuse above format
	columns[2] = ss.str();

	return columns;
}

void printSourceLine(const std::array<std::string, columnCount> & columns, const std::array<size_t, columnCount> & columnWidths)
{
	settings::output_stream
		<< "\t"
		<< std::right << std::setw(columnWidths[0])
		<< columns[0] << " "
		<< std::left << std::setw(columnWidths[1])
		<< columns[1];

	if (settings::runs > 1) {
		settings::output_stream
			<< "\t"
			<< "( +- " << std::right << std::setw(columnWidths[2])
			<< columns[2] << "% )";
	}
	settings::output_stream << std::endl;
}

void Experiment::printResult()
{
	if (settings::continuous_print_flag)
		return;

	settings::output_stream << "Energy counter stats for '";
	for (char **a = settings::workload_and_args; a && *a; ++a) {
		settings::output_stream << *a << " ";
	}

	settings::output_stream << "\b':" << std::endl;
	settings::output_stream << "[interval: " << settings::interval.count() << "ms, before: "
							                 << settings::before.count() << "ms, after: "
							                 << settings::after.count() << "ms, delay: "
							                 << settings::delay.count() << "ms, runs: "
							                 << settings::runs << "]" << std::endl;
	settings::output_stream << std::endl;

	std::vector<std::array<std::string, columnCount>> lines;

	for (size_t i = 0; i < settings::counters.size(); i++) {
		if (settings::energy_delayed_product) {
			lines.push_back(formatSourceLine(m_detail->edp_series_by_source[i], settings::counters[i]));
		} else {
			lines.push_back(formatSourceLine(m_detail->energy_series_by_source[i], settings::counters[i]));
		}
	}

	std::array<size_t, columnCount> columnWidths = {};
	for (const auto & line: lines) {
		for (size_t c = 0; c < line.size(); c++) {
			columnWidths[c] = std::max(columnWidths[c], line[c].size());
		}
	}

	for (const auto & line: lines) {
		printSourceLine(line, columnWidths);
	}

	settings::output_stream << std::endl;

	auto mean_time = meanAndStddevpercent<units::time::second>(m_detail->wall_times);
	settings::output_stream << "\t"
		<< std::fixed << std::setprecision(8)
		<< std::get<0>(mean_time).to<double>() << " seconds time elapsed ";
	if (settings::runs > 1) settings::output_stream
		<< std::fixed << std::setprecision(2)
		<< "( +- " << std::get<1>(mean_time) << "% )";
	settings::output_stream << std::endl;

	settings::output_stream << std::endl;
}

void Experiment::run_single()
{
	Sampler sampler(settings::interval, settings::counters);

	if (settings::before.count() > 0) {
		sampler.start();
		std::this_thread::sleep_for(settings::before);
	}

	auto start_time = std::chrono::high_resolution_clock::now();
	pid_t workload;
	if ((workload = fork())) {
		sampler.start(std::max(-settings::before, std::chrono::milliseconds(0)));
		waitpid(workload, NULL, 0);
	} else {
		if (settings::uid != settings::UID_NOT_SET)
			setuid(settings::uid);
		execvp(settings::workload_and_args[0], settings::workload_and_args);
	}

	auto end_time = std::chrono::high_resolution_clock::now();
	auto energy_by_source = sampler.stop(std::chrono::milliseconds(settings::after));

	m_detail->store_run(energy_by_source, as_unit_seconds(end_time - start_time));
}
