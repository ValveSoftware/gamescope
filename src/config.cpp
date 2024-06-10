#include "config.hpp"

#include <algorithm>
#include <cinttypes>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <unistd.h>

namespace gamescope {

static std::map<std::pair<std::string, uint16_t>, std::vector<uint32_t>> refresh_rates;
static const char config_dir_name[] = "/etc/gamescope.d";

// Parse the refresh_rates array
static int parse_refresh_rates(const Json::Value& rates, const std::string& vendor_id, uint16_t product_id)
{
	if (!rates.isArray())
	{
		fprintf(stderr, "Unexpected value: 'display_configuration.%s.%" PRIu16 ".refresh_rates' sound be an array\n",
			vendor_id.c_str(), product_id);
		return -1;
	}

	std::vector<uint32_t> v;
	v.reserve(rates.size());
	for (const auto& iter : rates)
	{
		if (!iter.isUInt() || iter.asUInt() > 0xffff)
		{
			fprintf(stderr, "Invalid value in 'display_configuration.%s.%" PRIu16 ".refresh_rates': '%s'\n",
				vendor_id.c_str(), product_id, iter.asString().c_str());
			return -1;
		}
		v.push_back(iter.asUInt());
	}
	// Overwrite any existing values (from e.g. a previous config file)
	refresh_rates.insert_or_assign(std::make_pair(vendor_id, product_id), std::move(v));

	return 0;
}

// Parse the 'display_configuration' setting
static int parse_display_config(const Json::Value& display_cfg)
{
	if (!display_cfg.isObject())
	{
		fprintf(stderr, "Unexpected value: 'display_configuration' should be an object\n");
		return -1;
	}

	for (const auto& vendor_id : display_cfg.getMemberNames())
	{
		const auto& vendor = display_cfg[vendor_id];
		if (!vendor.isObject())
		{
			fprintf(stderr, "Unexpected value: 'display_configuration.%s' should be an object\n",
				vendor_id.c_str());
			return -1;
		}

		for (const auto& product_id_str : vendor.getMemberNames())
		{
			char* endptr;
			unsigned long product_id = strtoul(product_id_str.c_str(), &endptr, 0);
			if (product_id_str.empty() || *endptr != '\0' || product_id > 0xffff)
			{
				fprintf(stderr, "Unexpected value: product ID '%s' in 'display_configuration.%s' should be a 16-bit integer\n",
					product_id_str.c_str(), vendor_id.c_str());
				return -1;
			}

			const auto& product = vendor[product_id_str];
			if (!product.isObject())
			{
				fprintf(stderr, "Unexpected value: 'display_configuration.%s.%s' should be an object\n",
					vendor_id.c_str(), product_id_str.c_str());
				return -1;
			}

			for (const auto& name : product.getMemberNames())
			{
				int ret = 0;
				if (name == "refresh_rates")
				{
					ret = parse_refresh_rates(product[name], vendor_id, product_id);
				}
				else
				{
					fprintf(stderr, "Unknown configuration setting 'display_configuration.%s.%s.%s'\n",
						vendor_id.c_str(), product_id_str.c_str(), name.c_str());
				}
				if (ret < 0)
					return ret;
			}
		}
	}

	return 0;
}

// Read a single configuration file
static int config_read_file(const std::string& file_name)
{
	std::ifstream file(file_name);
	if (!file.is_open())
	{
		fprintf(stderr, "Error opening config file '%s'\n", file_name.c_str());
		return -1;
	}

	Json::Value root;
	Json::CharReaderBuilder builder;
	std::string errors;
	if (!parseFromStream(builder, file, &root, &errors))
	{
		fprintf(stderr, "Error parsing config file '%s': %s\n",
			file_name.c_str(), errors.c_str());
		return -1;
	}

	if (!root.isObject())
	{
		fprintf(stderr, "Unexpected format in config file '%s'\n", file_name.c_str());
		return -1;
	}

	for (const auto& name : root.getMemberNames())
	{
		int ret = 0;
		if (name == "display_configuration")
		{
			ret = parse_display_config(root[name]);
		}
		else
		{
			fprintf(stderr, "Unknown configuration setting in '%s': '%s'\n",
				file_name.c_str(), name.c_str());
		}
		if (ret < 0)
			return ret;
	}

	return 0;
}

// Read all configuration files
int config_read()
{
	std::filesystem::path config_dir = config_dir_name;
	if (!std::filesystem::is_directory(config_dir))
		return 0;

	if (access(config_dir_name, R_OK | X_OK) != 0)
	{
		fprintf(stderr, "Cannot open directory '%s'\n", config_dir_name);
		return -1;
	}

	std::vector<std::string> files;
	for (const auto& iter : std::filesystem::directory_iterator(config_dir))
	{
		const auto& path = iter.path();
		if (std::filesystem::is_regular_file(iter.status()) && path.extension() == ".conf")
		{
			files.push_back(path);
		}
	}

	std::sort(files.begin(), files.end());
	for (const auto& path : files)
	{
		int ret = config_read_file(path);
		if (ret < 0)
			return ret;
	}

	return 0;
}

// Get the list of valid refresh rates for the specified display
std::span<const uint32_t> config_get_refresh_rates(const char *vendor_id, uint16_t product_id)
{
	const auto& rates = refresh_rates.find(std::make_pair(vendor_id, product_id));
	if (rates == refresh_rates.end())
		return {};
	else
		return rates->second;
}

} // namespace gamescope
