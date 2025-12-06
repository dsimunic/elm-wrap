/**
 * package_publish.h - Package publish command
 *
 * Implements the "package publish" command that uses rulr rules to
 * determine which files should be included when publishing a package.
 */

#ifndef PACKAGE_PUBLISH_H
#define PACKAGE_PUBLISH_H

/**
 * Run the package publish command.
 * 
 * Usage: wrap package publish <source-path>
 * 
 * This command uses the core_package_files.dl and publish_files.dl rules
 * to determine which files should be published, then prints a report.
 */
int cmd_package_publish(int argc, char *argv[]);

#endif /* PACKAGE_PUBLISH_H */
