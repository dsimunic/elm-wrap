/**
 * package_publish.h - Package prepublish command
 *
 * Implements the "package prepublish" command that uses rulr rules to
 * determine which files would be included when publishing a package.
 */

#ifndef PACKAGE_PUBLISH_H
#define PACKAGE_PUBLISH_H

/**
 * Run the package prepublish command.
 * 
 * Usage: wrap package prepublish PATH
 * 
 * This command uses the core_package_files and publish_files rules
 * to determine which files would be published, then prints a report.
 */
int cmd_package_prepublish(int argc, char *argv[]);

#endif /* PACKAGE_PUBLISH_H */
