add_test(
    NAME silhouette_scaffold_smoke
    COMMAND silhouette
)

set_tests_properties(
    silhouette_scaffold_smoke
    PROPERTIES PASS_REGULAR_EXPRESSION
               "Silhouette scaffold: V0.1 functionality is not implemented yet."
)
