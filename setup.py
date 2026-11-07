"""Build a platform wheel that carries the static binaries.

setuptools would otherwise tag the artifact py3-none-any, which pip would happily install on a
MacBook. Declaring the distribution as non-pure and forcing the tag to py3-none-<platform>
keeps it honest: this wheel only runs where the binaries do.
"""
from setuptools import Distribution, setup
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


class BinaryDistribution(Distribution):
    def has_ext_modules(self):  # noqa: D102 - tells setuptools the artifact is not pure python
        return True


class bdist_wheel(_bdist_wheel):
    def finalize_options(self):  # noqa: D102
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):  # noqa: D102
        _, _, platform = super().get_tag()
        return "py3", "none", platform


setup(distclass=BinaryDistribution, cmdclass={"bdist_wheel": bdist_wheel})
