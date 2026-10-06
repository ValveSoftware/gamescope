#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>

// Whether mangoapp would show its overlay, read the way mangoapp reads its config file
// and MANGOHUD_CONFIG, which replaces the file unless it sets read_cfg.
using mangoapp_options_t = std::unordered_map<std::string, std::string>;

static inline std::string_view mangoapp_config_trim( std::string_view s )
{
	const size_t start = s.find_first_not_of( " \t\r" );
	if ( start == std::string_view::npos )
		return {};
	const size_t end = s.find_last_not_of( " \t\r" );
	return s.substr( start, end - start + 1 );
}

static inline void mangoapp_config_parse( std::string_view sConfig, char cSeparator, mangoapp_options_t &options )
{
	while ( !sConfig.empty() )
	{
		size_t end = sConfig.find( cSeparator );
		std::string_view sLine = sConfig.substr( 0, end );
		sConfig = end == std::string_view::npos ? std::string_view{} : sConfig.substr( end + 1 );

		if ( size_t comment = sLine.find( '#' ); comment != std::string_view::npos )
			sLine = sLine.substr( 0, comment );

		size_t equal = sLine.find( '=' );
		std::string_view sParam = mangoapp_config_trim( sLine.substr( 0, equal ) );
		std::string_view sValue = equal == std::string_view::npos ? std::string_view{ "1" } : mangoapp_config_trim( sLine.substr( equal + 1 ) );
		if ( !sParam.empty() )
			options[ std::string{ sParam } ] = std::string{ sValue };
	}
}

static inline bool mangoapp_config_visible( std::string_view sFile, std::string_view sEnv = {} )
{
	mangoapp_options_t envOptions;
	mangoapp_config_parse( sEnv, ',', envOptions );

	mangoapp_options_t options;
	auto readCfg = envOptions.find( "read_cfg" );
	if ( sEnv.empty() || ( readCfg != envOptions.end() && readCfg->second != "0" ) )
		mangoapp_config_parse( sFile, '\n', options );
	for ( auto &iter : envOptions )
		options[ iter.first ] = iter.second;

	if ( auto iter = options.find( "no_display" ); iter != options.end() )
		return strtol( iter->second.c_str(), nullptr, 0 ) == 0;

	if ( auto iter = options.find( "preset" ); iter != options.end() )
	{
		// Only the first preset is live, the rest are the toggle_preset cycle.
		char *pEnd = nullptr;
		long nPreset = strtol( iter->second.c_str(), &pEnd, 10 );
		if ( pEnd != iter->second.c_str() && nPreset == 0 )
			return false;
	}

	return true;
}
