/* Round 10 (docs/port-ios/19-round10-xbla-import.md): the registrar half of
 * GoldenEye-Decomp-ios engine/port/include/config.h (dd80aeeb, at 55a3a9c8),
 * signatures unchanged, so the donor's texpack.c / beanstage.c compile here
 * as they are. NOT the donor's config.c: the implementation is
 * port/apple/xblaconfig.c, a few functions over Midas's midas.ini.
 * configLoad/configSave/GE_CONFIG and the options-overlay enumeration are
 * Midas's own business (port_options.c) and are not declared here.
 *
 * 64-bit host layer only (port/apple/, GE_HOST32=OFF). Host memory only:
 * a registered variable is plain host state, never the sim's (mema/memp)
 * and never read by the sim. */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register an integer option. min==max disables clamping. The variable's
 * initial value is the default; midas.ini overrides it at configLoad(). */
void configRegisterInt(const char *key, int *value, int min, int max);
/* Register a string option (buf must live for the program's lifetime). */
void configRegisterString(const char *key, char *value, int bufSize);

/* Read every registered key from midas.ini (the "Sect.Key" key is line
 * `Key=` of section `[sect]`, the section name lower-cased). Keys the file
 * does not have keep their defaults. Safe to call more than once; a key
 * registered after the last load is read when it registers. */
void configLoad(void);

/* Write one registered key's current value back to midas.ini, in place
 * (portIniWriteStrs: GE_WINDOW / GE_NOINISAVE runs never write). 1 = saved. */
int configSaveKey(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* PORT_CONFIG_H */
